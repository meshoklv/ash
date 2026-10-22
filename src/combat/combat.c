/* Ash combat runtime - see combat.h for the frame contract and docs/combat.md for the rules and their sources. */
#include "combat.h"

#include <math.h>
#include <string.h>

#define PI_F 3.14159265f
#define DEG(x) ((x) * (PI_F / 180.0f))

/* ------------------------------------------------------------------ tunables not found in the data [версия] */
#define POISE_RESET_S 5.0f       /* wiki: poise refills if not hit for ~5 s */
#define GUARD_HALF_ANGLE 75.0f   /* shield covers +-75 deg of the facing (EquipParamWeapon has no usable angle) */
#define KNOCKBACK_TIME 0.2f      /* AtkParam.knockbackDist is covered in this time */
#define ACTION_REGEN_DELAY 0.3f  /* no regen right after paying stamina when the anim has no TAE 225 window */
#define BULLET_R0 0.10f          /* expanding bullet starts at this radius (Bullet.InitialHitRadius of 22301) */
#define THROW_SNAP_DIST 1.0f     /* victim placed this far in front of the attacker if the snap dummy is missing */

/* ------------------------------------------------------------------ small vector helpers */
static void v_set(float *o, float x, float y, float z) { o[0] = x; o[1] = y; o[2] = z; }
static void v_sub(float *o, const float *a, const float *b) { o[0] = a[0] - b[0]; o[1] = a[1] - b[1]; o[2] = a[2] - b[2]; }
static float v_dot(const float *a, const float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static float clampf(float x, float lo, float hi) { return x < lo ? lo : x > hi ? hi : x; }

static void fwd_of(float yaw, float *f) { v_set(f, -sinf(yaw), 0.0f, -cosf(yaw)); }

/* model space -> world: T(pos) * Ry(yaw) */
static void to_world(const CbActor *a, const float *m, float *o)
{
    float s = sinf(a->yaw), c = cosf(a->yaw);
    float x = c * m[0] + s * m[2], z = -s * m[0] + c * m[2];
    o[0] = a->pos[0] + x; o[1] = a->pos[1] + m[1]; o[2] = a->pos[2] + z;
}

static void bone_point(const CbActor *a, int b, const float *local, float *model)
{
    const float *m = a->bones + 16 * b;   /* column-major: x axis, y axis, z axis, translation */
    model[0] = m[0] * local[0] + m[4] * local[1] + m[8] * local[2] + m[12];
    model[1] = m[1] * local[0] + m[5] * local[1] + m[9] * local[2] + m[13];
    model[2] = m[2] * local[0] + m[6] * local[1] + m[10] * local[2] + m[14];
}

/* closest points between segments p1q1 and p2q2 (Ericson, RTCD 5.1.9); returns squared distance */
static float seg_seg(const float *p1, const float *q1, const float *p2, const float *q2, float *c1, float *c2)
{
    float d1[3], d2[3], r[3], s, t;
    float a, e, f, c, b, denom;
    int i;
    v_sub(d1, q1, p1); v_sub(d2, q2, p2); v_sub(r, p1, p2);
    a = v_dot(d1, d1); e = v_dot(d2, d2); f = v_dot(d2, r);
    if (a <= 1e-9f && e <= 1e-9f) { s = t = 0.0f; }
    else if (a <= 1e-9f) { s = 0.0f; t = clampf(f / e, 0.0f, 1.0f); }
    else {
        c = v_dot(d1, r);
        if (e <= 1e-9f) { t = 0.0f; s = clampf(-c / a, 0.0f, 1.0f); }
        else {
            b = v_dot(d1, d2); denom = a * e - b * b;
            s = denom != 0.0f ? clampf((b * f - c * e) / denom, 0.0f, 1.0f) : 0.0f;
            t = (b * s + f) / e;
            if (t < 0.0f) { t = 0.0f; s = clampf(-c / a, 0.0f, 1.0f); }
            else if (t > 1.0f) { t = 1.0f; s = clampf((b - c) / a, 0.0f, 1.0f); }
        }
    }
    for (i = 0; i < 3; i++) { c1[i] = p1[i] + d1[i] * s; c2[i] = p2[i] + d2[i] * t; }
    v_sub(r, c1, c2);
    return v_dot(r, r);
}

float cb_capsule_dist(const float a0[3], const float a1[3], const float b0[3], const float b1[3])
{
    float c1[3], c2[3];
    return sqrtf(seg_seg(a0, a1, b0, b1, c1, c2));
}

/* ------------------------------------------------------------------ data queries */
uint32_t cb_anim_flags(const CbData *d, const CbAnim *an, float t)
{
    uint32_t m = 0;
    float f = t * CB_FPS;
    int i;
    if (!an) return 0;
    for (i = 0; i < an->n_state; i++) {
        const CbState *s = &d->state[an->state_first + i];
        /* windows are checked on accumulated time, not frame numbers (combat-ai notes 4.3); [start, end) with 1e-3
         * frame of slack for float error */
        if (f >= (float)s->start - 1e-3f && f < (float)s->end - 1e-3f) m |= 1u << s->bit;
    }
    for (i = 0; i < an->n_evt; i++) {
        const CbEvt *e = &d->evt[an->evt_first + i];
        /* SpEffect 30 「パリィ崩され状態（投げ条件）」: riposte allowed (hollow a2100 0-55 [проверено]) */
        if ((e->type == 66 || e->type == 67) && (int)e->arg == 30 && f >= e->start && f < e->end) m |= CB_ST_RIPOSTABLE;
    }
    return m;
}

static float evt_value(const CbData *d, const CbAnim *an, int type, float t, float def)
{
    float f = t * CB_FPS;
    int i;
    if (!an) return def;
    for (i = 0; i < an->n_evt; i++) {
        const CbEvt *e = &d->evt[an->evt_first + i];
        if (e->type == type && f >= e->start && f < e->end) return e->arg;
    }
    return def;
}

const CbAnim *cb_actor_anim(const CbWorld *w, const CbActor *a) { return cb_anim_find(w->data, a->kind, a->anim_id); }

static int dummy_index(const CbData *d, const CbActor *a, int id, int src)
{
    const CbKind *k = &d->kind[a->kind];
    int i;
    for (i = 0; i < k->n_dummy; i++) {
        const CbDummy *dm = &d->dummy[k->dummy_first + i];
        if (dm->id == id && dm->src == src) return i;
    }
    return -1;
}

static void dummy_model(const CbData *d, const CbActor *a, int i, float *model)
{
    const CbKind *k = &d->kind[a->kind];
    const CbDummy *dm = &d->dummy[k->dummy_first + i];
    int b = i < CB_MAX_DUMMY ? a->dmy_bone[i] : -1;
    if (a->bones && b >= 0 && b < a->n_bones) bone_point(a, b, dm->local, model);
    else memcpy(model, dm->model, sizeof(float) * 3);
}

int cb_dummy_world(const CbWorld *w, const CbActor *a, int id, int src, float out[3])
{
    float m[3];
    int i = dummy_index(w->data, a, id, src);
    if (i < 0) return 0;
    dummy_model(w->data, a, i, m);
    to_world(a, m, out);
    return 1;
}

/* attack dummy lookup: weapon-model dummies first for the player (AtkParam.hitSourceType 0 = right, 1 = left
 * [версия]), body dummies otherwise */
static int atk_point(const CbWorld *w, const CbActor *a, const CbAtk *atk, int id, float *out)
{
    if (id < 0) return 0;
    if (!a->npc) {
        int src = atk->hit_source == 1 ? 2 : 1;
        if (cb_dummy_world(w, a, id, src, out)) return 1;
    }
    return cb_dummy_world(w, a, id, 0, out);
}

static void hurt_world(const CbWorld *w, const CbActor *a, int i, float *p0, float *p1)
{
    const CbData *d = w->data;
    const CbHurt *h = &d->hurt[d->kind[a->kind].hurt_first + i];
    float m0[3], m1[3];
    static const float zero[3] = {0, 0, 0};
    int b0 = i < CB_MAX_HURT ? a->hurt_bone[i][0] : -1, b1 = i < CB_MAX_HURT ? a->hurt_bone[i][1] : -1;
    if (a->bones && b0 >= 0 && b1 >= 0 && b0 < a->n_bones && b1 < a->n_bones) {
        bone_point(a, b0, zero, m0);
        bone_point(a, b1, zero, m1);
    } else {
        memcpy(m0, h->model_a, sizeof m0);
        memcpy(m1, h->model_b, sizeof m1);
    }
    to_world(a, m0, p0);
    to_world(a, m1, p1);
}

/* ------------------------------------------------------------------ setup */
void cb_world_init(CbWorld *w, const CbData *d)
{
    memset(w, 0, sizeof *w);
    w->data = d;
}

int cb_world_add(CbWorld *w, CbActor *a)
{
    if (w->n_actor >= CB_MAX_ACTORS) return -1;
    w->actor[w->n_actor] = a;
    return w->n_actor++;
}

static void actor_common(CbActor *a)
{
    int i;
    a->throw_partner = -1;
    for (i = 0; i < CB_MAX_DUMMY; i++) a->dmy_bone[i] = -1;
    for (i = 0; i < CB_MAX_HURT; i++) a->hurt_bone[i][0] = a->hurt_bone[i][1] = -1;
}

void cb_actor_init_player(CbActor *a, const CbData *d)
{
    const CbPlayer *p = d->player;
    memset(a, 0, sizeof *a);
    actor_common(a);
    a->kind = cb_kind_find(d, "c0000");
    a->chr = 0;
    a->team = 0;
    if (!p) return;
    a->wep_r = cb_weapon_find(d, p->weapon_r);
    a->wep_l = cb_weapon_find(d, p->weapon_l);
    a->hp = a->hp_max = p->hp;
    a->sp = a->sp_max = p->stamina;
    a->sp_regen = p->stamina_regen;
    a->poise = a->poise_max = p->poise;
    a->def[0] = p->def_phys; a->def[1] = p->def_mag; a->def[2] = p->def_fire; a->def[3] = p->def_thun;
}

int cb_actor_init_npc(CbActor *a, const CbData *d, uint32_t npc_param_id, const char *kind_name)
{
    const CbNpc *n = cb_npc_find(d, npc_param_id);
    memset(a, 0, sizeof *a);
    actor_common(a);
    a->kind = cb_kind_find(d, kind_name);
    if (!n || a->kind < 0) return -1;
    a->npc = n;
    a->chr = (kind_name[1] - '0') * 1000 + (kind_name[2] - '0') * 100 + (kind_name[3] - '0') * 10 + (kind_name[4] - '0');
    a->team = 1;
    a->hp = a->hp_max = n->hp;
    a->sp = a->sp_max = n->stamina;
    a->sp_regen = n->stamina_regen;
    /* MaxPoise 0 (hollows): every hit staggers; the Asylum Demon has no hit reactions in its TAE, so its poise is
     * effectively infinite (handled by the missing react anims) */
    a->poise = a->poise_max = n->poise;
    a->def[0] = n->def_phys; a->def[1] = n->def_mag; a->def[2] = n->def_fire; a->def[3] = n->def_thun;
    return 0;
}

void cb_actor_bind(CbActor *a, const CbData *d, CbBoneLookup lookup, void *ctx)
{
    const CbKind *k;
    int i;
    if (a->kind < 0) return;
    k = &d->kind[a->kind];
    for (i = 0; i < k->n_dummy && i < CB_MAX_DUMMY; i++) {
        const CbDummy *dm = &d->dummy[k->dummy_first + i];
        a->dmy_bone[i] = dm->bone[0] && lookup ? (int16_t)lookup(ctx, dm->bone) : -1;
    }
    for (i = 0; i < k->n_hurt && i < CB_MAX_HURT; i++) {
        const CbHurt *h = &d->hurt[k->hurt_first + i];
        a->hurt_bone[i][0] = lookup ? (int16_t)lookup(ctx, h->bone_a) : -1;
        a->hurt_bone[i][1] = lookup ? (int16_t)lookup(ctx, h->bone_b) : -1;
    }
}

/* ------------------------------------------------------------------ damage */
float cb_damage_formula(float atk, float def)
{
    if (atk <= 0.0f) return 0.0f;
    if (def <= 0.0f) return atk;
    if (atk < def) return 0.4f * atk * atk * atk / (def * def) - 0.09f * atk * atk / def + 0.1f * atk;
    return atk - 0.79f * def * expf(-0.27f * def / atk);
}

static void attack_power(const CbActor *at, const CbAtk *atk, float out[4], float *poise, float *stam)
{
    if (atk->flags & CB_ATK_NPC || !at->wep_r) {
        float m = at->npc ? at->npc->atk_mult : 1.0f, ms = at->npc ? at->npc->stam_dmg_mult : 1.0f;
        out[0] = atk->atk_phys * m; out[1] = atk->atk_mag * m; out[2] = atk->atk_fire * m; out[3] = atk->atk_thun * m;
        if (poise) *poise = atk->atk_sa;
        if (stam) *stam = atk->atk_stam * ms;
    } else {
        /* player: weapon attack rating x AtkParam percentages. Left-hand attacks (shield bash, parry) use the
         * left weapon. */
        const CbWeapon *wp = atk->hit_source == 1 && at->wep_l ? at->wep_l : at->wep_r;
        out[0] = wp->ar[0] * atk->pct_phys / 100.0f;
        out[1] = wp->ar[1] * atk->pct_mag / 100.0f;
        out[2] = wp->ar[2] * atk->pct_fire / 100.0f;
        out[3] = wp->ar[3] * atk->pct_thun / 100.0f;
        if (poise) *poise = wp->base_sa * atk->pct_sa / 100.0f + atk->atk_sa;
        if (stam) *stam = wp->base_stam * atk->pct_stam / 100.0f + atk->atk_stam;
    }
    if (stam && (atk->flags & CB_ATK_NO_STAMINA)) *stam = 0.0f;
}

float cb_attack_damage(const CbWorld *w, const CbActor *at, const CbAtk *atk, const CbActor *v, float *poise_dmg,
                       float *stam_dmg)
{
    float p[4], dmg = 0.0f;
    int e;
    (void)w;
    attack_power(at, atk, p, poise_dmg, stam_dmg);
    for (e = 0; e < 4; e++) dmg += cb_damage_formula(p[e], v->def[e]);
    return dmg;
}

/* ------------------------------------------------------------------ events */
static CbEvent *push_ev(CbWorld *w, int type, int ai, int vi)
{
    CbEvent *e;
    if (w->n_ev >= CB_MAX_EVENTS) return 0;
    e = &w->ev[w->n_ev++];
    memset(e, 0, sizeof *e);
    e->type = type; e->attacker = ai; e->victim = vi;
    return e;
}

static uint32_t react(const CbWorld *w, const CbActor *a, int r) { return w->data->kind[a->kind].react[r]; }

static void kill(CbWorld *w, int ai, int vi, int play_anim)
{
    CbActor *v = w->actor[vi];
    CbEvent *e;
    if (v->dead) return;
    v->hp = 0.0f;
    v->dead = 1;
    if (play_anim) v->req_anim = react(w, v, CB_R_DEATH);
    e = push_ev(w, CB_EV_DEATH, ai, vi);
    if (e) { e->anim = v->req_anim; memcpy(e->pos, v->pos, sizeof e->pos); }
}

static uint32_t stagger_anim(const CbWorld *w, const CbActor *v, int dmg_level)
{
    /* AtkParam.dmgLevel -> hit reaction [версия]: <=1 small, 2 medium, 3-5 large, >=6 knockdown; fall back to
     * the nearest smaller reaction the kind has */
    int r = dmg_level <= 1 ? CB_R_HIT_S : dmg_level == 2 ? CB_R_HIT_M : dmg_level <= 5 ? CB_R_HIT_L : CB_R_KNOCKDOWN;
    for (; r >= CB_R_HIT_S; r--)
        if (react(w, v, r)) return react(w, v, r);
    return 0;
}

static int in_front(const CbActor *v, const CbActor *from, float half_deg)
{
    float f[3], d[3], l;
    fwd_of(v->yaw, f);
    v_sub(d, from->pos, v->pos);
    d[1] = 0.0f;
    l = sqrtf(v_dot(d, d));
    if (l < 1e-4f) return 1;
    return v_dot(f, d) / l >= cosf(DEG(half_deg));
}

/* Repel (weapon bounce off a shield) [версия: rule from the field names, combat-ai notes 4.1 "guardBaseRepel /
 * attackBaseRepel"; DS1's exact comparison is not in the data]: the attack bounces when its repel power is below the
 * shield's EquipParamWeapon.DeflectOnBlock. Repel power: NPC = AtkParam_Npc.GuardAttackPower (hollow 20, Asylum Demon
 * 100); player = weapon DeflectOnAttack x AtkParam_Pc.GuardAttackPercentage % (Longsword 20 x 100 %; plunge and jump
 * attacks 9900 % never bounce). Knight Shield DeflectOnBlock 50: hollow swings bounce, the demon's do not. */
static int repel_power(const CbActor *a, const CbAtk *atk)
{
    const CbWeapon *wp;
    if (atk->flags & CB_ATK_NPC || !a->wep_r) return atk->guard_atk_rate;
    wp = atk->hit_source == 1 && a->wep_l ? a->wep_l : a->wep_r;
    return (int)((float)wp->deflect_attack * atk->pct_guard_atk / 100.0f);
}

static void resolve_hit(CbWorld *w, int ai, int vi, const CbAtk *atk, const float *pos, int atk_type)
{
    CbActor *a = w->actor[ai], *v = w->actor[vi];
    CbEvent *e;
    float p[4], poise = 0, stam = 0, dmg = 0, dir[3], l, knock;
    int el, guarded;
    if ((v->flags & (CB_ST_INVINCIBLE | CB_ST_INV_EXC_THROW | CB_ST_THROWN)) && !(atk->flags & CB_ATK_IGNORE_IFRAMES)) {
        e = push_ev(w, CB_EV_INVULN, ai, vi);
        if (e) memcpy(e->pos, pos, sizeof e->pos);
        return;
    }
    attack_power(a, atk, p, &poise, &stam);
    guarded = !(atk->flags & CB_ATK_IGNORE_GUARD) && v->wep_l && (v->wep_l->flags & 2) &&
              (v->guard_input || (v->flags & CB_ST_GUARD)) && in_front(v, a, GUARD_HALF_ANGLE);
    for (el = 0; el < 4; el++) {
        float d = cb_damage_formula(p[el], v->def[el]);
        if (guarded) d *= 1.0f - v->wep_l->guard_cut[el] / 100.0f;
        dmg += d;
    }
    knock = atk->knockback * evt_value(w->data, cb_actor_anim(w, a), 226, a->anim_t, 100.0f) / 100.0f;
    v_sub(dir, v->pos, a->pos);
    dir[1] = 0.0f;
    l = sqrtf(v_dot(dir, dir));
    if (l > 1e-4f) { dir[0] /= l; dir[2] /= l; } else fwd_of(a->yaw, dir);
    a->hitstop = atk->hitstop;
    v->hitstop = atk->hitstop;
    if (guarded) {
        /* stamina loss = attack stamina damage x (1 - stability%) [версия, wiki] */
        float loss = stam * (1.0f - v->wep_l->stability / 100.0f);
        v->sp -= loss;
        v->hp -= dmg;
        v->regen_block = ACTION_REGEN_DELAY;
        if (v->sp < 0.0f) {
            v->sp = 0.0f;
            v->req_anim = react(w, v, CB_R_GUARD_BREAK);
            e = push_ev(w, CB_EV_GUARD_BREAK, ai, vi);
        } else {
            v->req_anim = react(w, v, atk->dmg_level >= 3 ? CB_R_GUARD_HIT_L : CB_R_GUARD_HIT);
            e = push_ev(w, CB_EV_BLOCK, ai, vi);
        }
        if (e) {
            e->damage = dmg; e->stamina = loss; e->anim = v->req_anim; e->atk_type = (uint8_t)atk_type;
            memcpy(e->pos, pos, sizeof e->pos);
        }
        if (v->sp > 0.0f && repel_power(a, atk) < v->wep_l->deflect_block && react(w, a, CB_R_REPELLED) && !a->dead) {
            a->req_anim = react(w, a, CB_R_REPELLED);
            a->hitstop = atk->hitstop > 0.1f ? atk->hitstop : 0.1f;   /* [версия] short clash freeze */
            e = push_ev(w, CB_EV_REPEL, ai, vi);
            if (e) { e->anim = a->req_anim; e->atk_type = (uint8_t)atk_type; memcpy(e->pos, pos, sizeof e->pos); }
        }
        /* reduced knockback on block [версия] */
        v->push_t = KNOCKBACK_TIME;
        v_set(v->push, dir[0] * knock * 0.5f / KNOCKBACK_TIME, 0, dir[2] * knock * 0.5f / KNOCKBACK_TIME);
        if (v->hp <= 0.0f) kill(w, ai, vi, 1);
        return;
    }
    v->hp -= dmg;
    e = push_ev(w, CB_EV_HIT, ai, vi);
    if (e) { e->damage = dmg; e->atk_type = (uint8_t)atk_type; memcpy(e->pos, pos, sizeof e->pos); }
    v->push_t = KNOCKBACK_TIME;
    v_set(v->push, dir[0] * knock / KNOCKBACK_TIME, 0, dir[2] * knock / KNOCKBACK_TIME);
    if (v->hp <= 0.0f) { kill(w, ai, vi, 1); return; }
    /* poise: hyper armor (TAE flag 24) prevents the stagger [версия: DS1 keeps poise damage, we ignore it] */
    v->poise_timer = 0.0f;
    if ((v->flags & CB_ST_SUPER_ARMOR) && !(v->flags & CB_ST_NO_SUPER_ARMOR)) return;
    v->poise -= poise;
    if (v->poise <= 0.0f) {
        uint32_t an = stagger_anim(w, v, atk->dmg_level);
        v->poise = v->poise_max;
        if (an) {
            v->req_anim = an;
            e = push_ev(w, CB_EV_STAGGER, ai, vi);
            if (e) { e->anim = an; memcpy(e->pos, pos, sizeof e->pos); }
        }
    }
}

/* parry hitbox of `ai` touched `vi`: succeeds if vi's current attack is in its parry-possible window (TAE flag 5) */
static int resolve_parry(CbWorld *w, int ai, int vi, const float *pos)
{
    CbActor *v = w->actor[vi];
    uint32_t an;
    CbEvent *e;
    if (!(v->flags & CB_ST_PARRYABLE) || v->dead) return 0;
    an = react(w, v, CB_R_PARRIED);
    if (!an) return 0;
    v->req_anim = an;
    v->poise = v->poise_max;
    e = push_ev(w, CB_EV_PARRY, ai, vi);
    if (e) { e->anim = an; memcpy(e->pos, pos, sizeof e->pos); }
    return 1;
}

/* ------------------------------------------------------------------ hit windows */
static int shape_points(const CbWorld *w, const CbActor *a, const CbAtk *atk, const CbHit *h, int s, float t,
                        float *p0, float *p1, float *r)
{
    if (h->kind == CB_HK_BULLET) {
        float span = (float)(h->end - h->start), k;
        if (s > 0) return 0;
        if (!cb_dummy_world(w, a, h->bullet_dmy, 0, p0)) return 0;
        memcpy(p1, p0, sizeof(float) * 3);
        k = span > 0 ? clampf((t * CB_FPS - h->start) / span, 0.0f, 1.0f) : 1.0f;
        *r = BULLET_R0 + (h->bullet_r_cm / 100.0f - BULLET_R0) * k;
        if (atk->radius[0] > *r) *r = atk->radius[0];
        return 1;
    }
    if (atk->radius[s] <= 0.0f || atk->dmy_a[s] < 0) return 0;
    if (!atk_point(w, a, atk, atk->dmy_a[s], p0)) return 0;
    if (atk->dmy_b[s] >= 0 && atk->dmy_b[s] != atk->dmy_a[s]) {
        if (!atk_point(w, a, atk, atk->dmy_b[s], p1)) memcpy(p1, p0, sizeof(float) * 3);
    } else memcpy(p1, p0, sizeof(float) * 3);
    *r = atk->radius[s];
    return 1;
}

static int touch(const CbWorld *w, const CbActor *v, const float *a0, const float *a1, float r, float *contact)
{
    const CbData *d = w->data;
    const CbKind *k = &d->kind[v->kind];
    float h0[3], h1[3], c1[3], c2[3];
    int i;
    if (k->n_hurt == 0 && v->npc) {   /* fallback: upright capsule from NpcParam hit radius/height */
        v_set(h0, v->pos[0], v->pos[1] + v->npc->hit_radius, v->pos[2]);
        v_set(h1, v->pos[0], v->pos[1] + v->npc->hit_height - v->npc->hit_radius, v->pos[2]);
        if (seg_seg(a0, a1, h0, h1, c1, c2) <= (r + v->npc->hit_radius) * (r + v->npc->hit_radius)) {
            memcpy(contact, c2, sizeof c2); return 1;
        }
        return 0;
    }
    for (i = 0; i < k->n_hurt; i++) {
        float rr = r + d->hurt[k->hurt_first + i].radius;
        hurt_world(w, v, i, h0, h1);
        if (seg_seg(a0, a1, h0, h1, c1, c2) <= rr * rr) { memcpy(contact, c2, sizeof c2); return 1; }
    }
    return 0;
}

static void run_windows(CbWorld *w, int ai)
{
    CbActor *a = w->actor[ai];
    const CbData *d = w->data;
    const CbAnim *an = cb_actor_anim(w, a);
    float f0, f1;
    int k, s, vi;
    if (!an || a->dead) return;
    f0 = a->anim_t_prev * CB_FPS;
    f1 = a->anim_t * CB_FPS;
    if (f1 < f0) f0 = 0.0f;   /* looped/restarted */
    for (k = 0; k < an->n_hit && k < CB_MAX_WIN; k++) {
        const CbHit *h = &d->hit[an->hit_first + k];
        const CbAtk *atk;
        float cur[4][2][3], rad[4];
        int have[4], any = 0;
        if (h->kind == CB_HK_THROW || h->atk >= d->n_atk) continue;
        if (!(f1 >= h->start && f0 < h->end)) { a->prev_valid[k] = 0; continue; }
        atk = &d->atk[h->atk];
        for (s = 0; s < 4; s++) {
            have[s] = shape_points(w, a, atk, h, s, a->anim_t, cur[s][0], cur[s][1], &rad[s]);
            any |= have[s];
        }
        if (!any) continue;
        for (vi = 0; vi < w->n_actor; vi++) {
            CbActor *v = w->actor[vi];
            float contact[3];
            int hit = 0;
            if (v == a || v->team == a->team || v->dead || (a->win_hit[k] >> vi & 1)) continue;
            if (h->attack_type == CB_ATTACK_PLUNGE && (a->plunge_hit >> vi & 1)) continue;
            for (s = 0; s < 4 && !hit; s++) {
                if (!have[s]) continue;
                hit = touch(w, v, cur[s][0], cur[s][1], rad[s], contact);
                /* swept test against last frame's end points: fast swings at 30 Hz must not tunnel */
                if (!hit && a->prev_valid[k]) {
                    hit = touch(w, v, a->prev_tip[k][s], cur[s][1], rad[s], contact) ||
                          touch(w, v, a->prev_tip[k][s], cur[s][0], rad[s], contact);
                }
            }
            if (!hit) continue;
            /* the parry box keeps testing over its whole window until the enemy enters parry-possible frames */
            if (h->attack_type == CB_ATTACK_PARRY) {
                if (resolve_parry(w, ai, vi, contact)) a->win_hit[k] |= 1u << vi;
            } else {
                a->win_hit[k] |= 1u << vi;
                if (h->attack_type == CB_ATTACK_PLUNGE) a->plunge_hit |= 1u << vi;
                resolve_hit(w, ai, vi, atk, contact, h->attack_type);
            }
        }
        for (s = 0; s < 4; s++) if (have[s]) memcpy(a->prev_tip[k][s], cur[s][1], sizeof(float) * 3);
        a->prev_valid[k] = 1;
    }
}

/* ------------------------------------------------------------------ throws */
static const CbHit *throw_hit(const CbData *d, const CbAnim *an)
{
    int i;
    if (!an) return 0;
    for (i = 0; i < an->n_hit; i++)
        if (d->hit[an->hit_first + i].kind == CB_HK_THROW) return &d->hit[an->hit_first + i];
    return 0;
}

static float wrap_deg(float a) { while (a > 180.0f) a -= 360.0f; while (a < -180.0f) a += 360.0f; return a; }

int cb_try_throw(CbWorld *w, int ai)
{
    const CbData *d = w->data;
    CbActor *a = w->actor[ai];
    int pass, vi;
    if (a->dead || !a->wep_r || a->throw_partner >= 0) return -1;
    for (pass = 1; pass >= 0; pass--) {          /* 1 = riposte first, then backstab */
        for (vi = 0; vi < w->n_actor; vi++) {
            CbActor *v = w->actor[vi];
            const CbThrow *t;
            float dv[3], dist, ang_to, face_diff, dy;
            const CbAnim *atk_an;
            const CbHit *th;
            uint32_t atk_anim;
            if (v == a || v->team == a->team || v->dead || v->throw_partner >= 0) continue;
            if (pass == 1 && !(v->flags & CB_ST_RIPOSTABLE)) continue;
            if (pass == 0 && (v->flags & (CB_ST_RIPOSTABLE | CB_ST_INVINCIBLE))) continue;
            t = cb_throw_find(d, a->chr, v->chr, pass);
            if (!t) continue;
            v_sub(dv, v->pos, a->pos);
            dy = dv[1];
            dv[1] = 0.0f;
            dist = sqrtf(v_dot(dv, dv));
            /* ThrowParam.Dist measured centre to centre [версия]; vertical window upper/lowerYRange */
            if (dist > t->dist || dy > t->y_up || dy < -t->y_low) continue;
            ang_to = dist > 1e-4f ? wrap_deg((atan2f(-dv[0], -dv[2]) - a->yaw) * 180.0f / PI_F) : 0.0f;
            if (fabsf(ang_to) > t->ang_to_def) continue;
            face_diff = fabsf(wrap_deg((v->yaw - a->yaw) * 180.0f / PI_F));
            if (face_diff < t->ang_min || face_diff > t->ang_max) continue;
            atk_anim = (uint32_t)a->wep_r->atk_cat * 10000u + t->atk_anim;
            atk_an = cb_anim_find(d, a->kind, atk_anim);
            th = throw_hit(d, atk_an);
            if (!th || th->atk >= d->n_atk) continue;
            /* go */
            a->req_anim = atk_anim;
            v->req_anim = t->def_anim;
            a->throw_partner = vi; v->throw_partner = ai;
            a->throw_atk = &d->atk[th->atk];
            a->throw_atk_t = th->start / CB_FPS;
            a->throw_done = 0;
            v->flags |= CB_ST_THROWN;
            if (pass == 1) v->ripostable_t = 0.0f;
            {   /* snap the victim to the attacker's throw dummy (230 backstab / 231 riposte) */
                float sp[3], f[3];
                if (!(t->atk_dmy > 0 && cb_dummy_world(w, a, t->atk_dmy, 0, sp))) {
                    fwd_of(a->yaw, f);
                    v_set(sp, a->pos[0] + f[0] * THROW_SNAP_DIST, a->pos[1], a->pos[2] + f[2] * THROW_SNAP_DIST);
                }
                v->pos[0] = sp[0]; v->pos[2] = sp[2];
                v->yaw = pass == 1 ? a->yaw + PI_F : a->yaw;   /* riposte: face to face; backstab: same facing */
            }
            {
                CbEvent *e;
                if (!w->ev_ext) { w->n_ev = 0; w->ev_ext = 1; }   /* the previous update's events were consumed */
                e = push_ev(w, CB_EV_THROW, ai, vi);
                if (e) { e->anim = v->req_anim; memcpy(e->pos, v->pos, sizeof e->pos); }
            }
            return vi;
        }
    }
    return -1;
}

static void run_throw(CbWorld *w, int ai)
{
    CbActor *a = w->actor[ai];
    CbActor *v;
    CbEvent *e;
    float dmg;
    if (a->throw_partner < 0 || !a->throw_atk) return;
    v = w->actor[a->throw_partner];
    if (a->req_anim) return;                        /* game has not started the throw anim yet */
    if (a->throw_done || a->anim_t < a->throw_atk_t) return;
    dmg = cb_attack_damage(w, a, a->throw_atk, v, 0, 0);
    v->hp -= dmg;
    a->throw_done = 1;
    e = push_ev(w, CB_EV_THROW_HIT, ai, a->throw_partner);
    if (e) { e->damage = dmg; memcpy(e->pos, v->pos, sizeof e->pos); }
    /* a riposte/backstab kill: the victim finishes its throw anim, the game plays death after it [версия] */
    if (v->hp <= 0.0f) kill(w, ai, a->throw_partner, 0);
}

static void end_throw(CbWorld *w, CbActor *a)
{
    if (a->throw_partner >= 0) {
        CbActor *v = w->actor[a->throw_partner];
        if (v->throw_partner >= 0 && w->actor[v->throw_partner] == a) v->throw_partner = -1;
    }
    a->throw_partner = -1;
    a->throw_atk = 0;
}

uint32_t cb_attack_anim(const CbWorld *w, const CbActor *a, int action, int two_handed)
{
    uint32_t id;
    if (!a->wep_r || action <= 0) return 0;
    id = (uint32_t)a->wep_r->atk_cat * 10000u + (uint32_t)action + (two_handed ? 1000u : 0u);
    return cb_anim_find(w->data, a->kind, id) ? id : 0;
}

/* ------------------------------------------------------------------ per frame */
int cb_begin_anim(CbWorld *w, CbActor *a, uint32_t anim_id)
{
    const CbData *d = w->data;
    const CbAnim *an = cb_anim_find(d, a->kind, anim_id);
    float cost = an ? an->stamina : 0.0f;
    if (a->dead) return 0;
    if (!a->npc && d->player && anim_id >= 680 && anim_id < 744)
        cost = anim_id < 700 ? d->player->backstep_stamina : d->player->roll_stamina;
    if (cost > 0.0f && !a->npc) {
        if (a->sp <= 0.0f) return 0;    /* DS1: an action needs stamina > 0 */
        a->sp -= cost;
        if (a->sp < 0.0f) a->sp = 0.0f;
        /* attacks/rolls stop regen through their own TAE 225 windows; others get a short pause [версия] */
        if (evt_value(d, an, 225, 0.0f, -1.0f) < 0.0f) a->regen_block = ACTION_REGEN_DELAY;
    }
    a->win_anim = anim_id;
    memset(a->win_hit, 0, sizeof a->win_hit);
    memset(a->prev_valid, 0, sizeof a->prev_valid);
    return 1;
}

void combat_update(CbWorld *w, float dt)
{
    const CbData *d = w->data;
    int i;
    if (!w->ev_ext) w->n_ev = 0;   /* a THROW pushed by cb_try_throw since the last update stays in this list */
    w->ev_ext = 0;
    w->time += dt;
    for (i = 0; i < w->n_actor; i++) {
        CbActor *a = w->actor[i];
        const CbAnim *an = cb_actor_anim(w, a);
        uint32_t keep = a->flags & CB_ST_THROWN;
        float mult;
        if (a->anim_id != a->win_anim || a->anim_t < a->anim_t_prev) {   /* anim changed without cb_begin_anim */
            a->win_anim = a->anim_id;
            memset(a->win_hit, 0, sizeof a->win_hit);
            memset(a->prev_valid, 0, sizeof a->prev_valid);
        }
        a->flags = cb_anim_flags(d, an, a->anim_t);
        if (a->plunge_hit) {   /* the plunge ends with the first anim that has no plunge (AttackType 62) window */
            int k, pl = 0;
            for (k = 0; an && k < an->n_hit; k++) pl |= d->hit[an->hit_first + k].attack_type == CB_ATTACK_PLUNGE;
            if (!pl) a->plunge_hit = 0;
        }
        if (a->throw_partner >= 0) a->flags |= keep;
        if (a->flags & CB_ST_RIPOSTABLE) a->ripostable_t = 0.1f;   /* SpEffect 30 lasts 0.1 s after the window */
        else if (a->ripostable_t > 0.0f) { a->ripostable_t -= dt; a->flags |= CB_ST_RIPOSTABLE; }
        if (a->hitstop > 0.0f) a->hitstop -= dt;
        if (a->push_t > 0.0f) { a->push_t -= dt; if (a->push_t <= 0.0f) v_set(a->push, 0, 0, 0); }
        if (a->dead) continue;
        /* poise refill */
        a->poise_timer += dt;
        if (a->poise_timer >= POISE_RESET_S) a->poise = a->poise_max;
        /* stamina: TAE 225 sets the regen percentage inside its window (0 during attacks) */
        mult = evt_value(d, an, 225, a->anim_t, -1.0f);
        if (mult >= 0.0f) mult /= 100.0f;      /* guard anims carry 225 = 20% themselves (a00_0001) [проверено] */
        else if (!a->npc && d->player && a->guard_input) mult = d->player->guard_regen_mult;   /* shield up, anim without 225 */
        else mult = 1.0f;
        if (a->regen_block > 0.0f) { a->regen_block -= dt; mult = 0.0f; }
        a->sp += a->sp_regen * mult * dt;
        if (a->sp > a->sp_max) a->sp = a->sp_max;
    }
    for (i = 0; i < w->n_actor; i++) run_windows(w, i);
    for (i = 0; i < w->n_actor; i++) {
        CbActor *a = w->actor[i];
        if (a->throw_partner >= 0 && a->throw_atk) {
            const CbAnim *an = cb_actor_anim(w, a);
            run_throw(w, i);
            /* the attacker left its throw anim (finished or cancelled): release both */
            if (!a->req_anim && an && !throw_hit(d, an)) {
                CbActor *v = w->actor[a->throw_partner];
                v->flags &= ~CB_ST_THROWN;
                end_throw(w, a);
            }
        }
        a->anim_t_prev = a->anim_t;
    }
}

int combat_debug_shapes(const CbWorld *w, CbShape *out, int max)
{
    const CbData *d = w->data;
    int n = 0, i, k, s;
    for (i = 0; i < w->n_actor && n < max; i++) {
        const CbActor *a = w->actor[i];
        const CbKind *kd = &d->kind[a->kind];
        const CbAnim *an = cb_actor_anim(w, a);
        for (k = 0; k < kd->n_hurt && n < max; k++, n++) {
            hurt_world(w, a, k, out[n].a, out[n].b);
            out[n].r = d->hurt[kd->hurt_first + k].radius;
            out[n].kind = 0;
        }
        if (!an) continue;
        for (k = 0; k < an->n_hit; k++) {
            const CbHit *h = &d->hit[an->hit_first + k];
            float f = a->anim_t * CB_FPS;
            if (h->atk >= d->n_atk || f < h->start || f >= h->end) continue;
            for (s = 0; s < 4 && n < max; s++)
                if (shape_points(w, a, &d->atk[h->atk], h, s, a->anim_t, out[n].a, out[n].b, &out[n].r)) {
                    out[n].kind = 1; n++;
                }
        }
    }
    return n;
}
