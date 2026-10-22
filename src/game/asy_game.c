/*
 * Ash "Asylum" simulation (asy.h): player, hollows, the Asylum Demon, events host, combat, progression.
 * Platform-independent: no drawing here (asy_main.c renders on the PSP, tests/native runs it without a GPU).
 * Rules owned by the modules are not duplicated: HP/stamina/hits = src/combat, flags/doors/fog/respawn = src/event,
 * Estus/souls/death screen/rest = src/progress, the demon's body/AI = src/boss + its Lua script.
 * What is ours here is tagged [версия]/[придумал] in docs/asylum.md.
 */
#include "asy.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "save.h"

const char *const asy_pst_name[P_COUNT] = {"idle", "walk", "jog", "run", "roll", "backstep", "attack", "guard",
                                           "parry", "throw", "react", "prg", "fog", "fall", "plunge", "ladder"};

static Game *G;   /* one game; AI / event callbacks reach it here */

static const PcolCapsule CAP = {0.35f, 1.8f, 0.45f, 0.64f, 0.35f, 9.8f};   /* [версия] as src/game M4 */
static const PcolCapsule HCAP = {0.35f, 1.7f, 0.45f, 0.64f, 0.35f, 9.8f};  /* hollow: NpcParam hit radius 0.4 */
static const float BLEND = 5.0f / 30.0f, BUFFER_TIME = 0.25f;
static const u32 KEY_ESTUS = (4u << 28) | 200u;

/* ------------------------------------------------------------------ helpers */

float asy_wrap_pi(float a)
{
    while (a > ASY_PI) a -= 2 * ASY_PI;
    while (a < -ASY_PI) a += 2 * ASY_PI;
    return a;
}
void asy_fs(float x, float y, float z, float out[3]) { out[0] = -x; out[1] = y; out[2] = z; }
float asy_flat_dist(const float a[3], const float b[3])
{
    float dx = a[0] - b[0], dz = a[2] - b[2];
    return sqrtf(dx * dx + dz * dz);
}
int asy_has_estus(const Game *g)
{
    if (g->has_items && g->class_row) return itm_has(&g->inv, KEY_ESTUS) || itm_has(&g->inv, KEY_ESTUS + 1);
    return g->has_ev && ev_has_item(&g->ev, KEY_ESTUS);
}

void asy_log(Game *g, const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    printf("ash-asy: f%u %s\n", (unsigned)g->frame, buf);
}

static float yaw_to(const float from[3], const float to[3])
{
    return atan2f(-(to[0] - from[0]), -(to[2] - from[2]));
}

static float turn_toward(float yaw, float want, float max_step)
{
    float d = asy_wrap_pi(want - yaw);
    return asy_wrap_pi(yaw + (d > max_step ? max_step : d < -max_step ? -max_step : d));
}

/* ------------------------------------------------------------------ clips and bodies */

static ClipRef clip_name(const AnimSet *s, const char *name)
{
    ClipRef r = {s, -1};
    if (s && s->hdr) r.idx = anim_find(s, name);
    if (r.idx < 0) r.set = NULL;
    return r;
}

static ClipRef clip_id(const AnimSet *s, u32 id)
{
    ClipRef r = {NULL, -1};
    if (!s || !s->hdr) return r;
    for (u32 i = 0; i < s->hdr->n_clips; i++)
        if (s->clips[i].anim_id == id) { r.set = s; r.idx = (int)i; return r; }
    return r;
}

static int clip_ok(ClipRef r) { return r.set && r.idx >= 0; }
static float clip_dur(ClipRef r) { return clip_ok(r) ? anim_duration(r.set, r.idx) : 0; }
static int clip_loops(ClipRef r) { return clip_ok(r) && (r.set->clips[r.idx].flags & CLIP_LOOP); }
static u32 clip_anim_id(ClipRef r) { return clip_ok(r) ? r.set->clips[r.idx].anim_id : 0; }
static int clip_same(ClipRef a, ClipRef b) { return a.set == b.set && a.idx == b.idx; }

static void body_play(Body *b, ClipRef r, float blend, int restart)
{
    if (!clip_ok(r)) return;
    if (!restart && clip_same(b->cur, r)) return;
    if (clip_ok(b->cur)) { b->prev = b->cur; b->pt = b->t; }
    b->cur = r;
    b->t = 0;
    b->blend = 0;
    b->blend_len = blend;
    b->hold = 0;
}

static float body_time(const Body *b, ClipRef r, float t)
{
    float d = clip_dur(r);
    if (clip_loops(r) && !b->hold) return d > 0 ? fmodf(t, d) : 0;
    return t > d ? d : t;
}

static int body_done(const Body *b, float early) { return b->t >= clip_dur(b->cur) - early; }

static void body_root(const Body *b, float dt, float out[4])
{
    out[0] = out[1] = out[2] = out[3] = 0;
    if (!clip_ok(b->cur)) return;
    float d = clip_dur(b->cur);
    if (!clip_loops(b->cur) && b->t >= d) return;
    anim_root_delta(b->cur.set, b->cur.idx, b->t, b->t + dt, out);
    float w = b->blend_len > 0 && b->blend < b->blend_len ? b->blend / b->blend_len : 1.0f;
    if (w < 1.0f && clip_ok(b->prev)) {
        float e[4] = {0};
        float pd = clip_dur(b->prev);
        if (clip_loops(b->prev) || b->pt < pd) anim_root_delta(b->prev.set, b->prev.idx, b->pt, b->pt + dt, e);
        for (int i = 0; i < 4; i++) out[i] = out[i] * w + e[i] * (1 - w);
    }
}

static void body_advance(Body *b, float dt)
{
    b->t += dt;
    b->pt += dt;
    b->blend += dt;
}

static void body_pose(Body *b)
{
    if (!clip_ok(b->cur)) return;
    int nb = b->cur.set->n_bones;
    anim_sample(b->cur.set, b->cur.idx, body_time(b, b->cur, b->t), b->pa);
    float w = b->blend_len > 0 && b->blend < b->blend_len ? b->blend / b->blend_len : 1.0f;
    if (w < 1.0f && clip_ok(b->prev)) {
        anim_sample(b->prev.set, b->prev.idx, body_time(b, b->prev, b->pt), b->pb);
        anim_blend(nb, b->pb, b->pa, w, b->pa);
    }
    anim_skin_matrices(nb, b->chr->parent, b->pa, b->chr->inv_bind, b->world, b->skin);
}

static int bone_lookup(void *ctx, const char *name) { return chr_bone((const Chr *)ctx, name); }

/* ------------------------------------------------------------------ damage outside src/combat */

static void player_die(Game *g, const char *why)
{
    Player *p = &g->pl;
    if (p->dead) return;
    p->dead = 1;
    g->pact.hp = 0;
    g->pact.dead = 1;
    g->deaths++;
    asy_log(g, "PLAYER DIED (%s) at (%.2f %.2f %.2f), souls %d -> bloodstain", why, p->pos[0], p->pos[1], p->pos[2],
            (int)g->prg.souls);
    prg_player_died(&g->prg, p->safe_pos);
    if (clip_ok(g->k_death)) { body_play(&g->pb, g->k_death, BLEND, 1); g->pb.hold = 1; }
    p->st = P_PRG;
    g->lock = -1;
}

/* A hit from something that is not a combat actor (the demon's swing volumes, the iron ball, a firebomb).
 * Uses the combat module's numbers: i-frames from the roll's TAE flags, the shield's guard cut and stability,
 * the DS1 damage formula. Reaction clips: hit_m / hit_l / knockdown by impact [версия]. */
static void player_hit_external(Game *g, float phys, float fire, float stam_dmg, int impact, const float from[3],
                                int ignore_guard, const char *what)
{
    Player *p = &g->pl;
    CbActor *a = &g->pact;
    if (p->dead) return;
    if (a->flags & CB_ST_INVINCIBLE) {
        g->rolls_iframe++;
        asy_log(g, "%s: dodged inside i-frames (%s)", what, asy_pst_name[p->st]);
        return;
    }
    float dmg = cb_damage_formula(phys, a->def[0]) + (fire > 0 ? cb_damage_formula(fire, a->def[2]) : 0);
    float face = yaw_to(p->pos, from), front = fabsf(asy_wrap_pi(face - p->yaw));
    int guarding = (a->guard_input || (a->flags & CB_ST_GUARD)) && !ignore_guard && front < 75.0f * ASY_PI / 180;
    if (guarding && a->wep_l) {
        float cut = a->wep_l->guard_cut[0] / 100.0f;
        dmg = cb_damage_formula(phys * (1 - cut), a->def[0]) + (fire > 0 ? cb_damage_formula(fire * (1 - a->wep_l->guard_cut[2] / 100.0f), a->def[2]) : 0);
        float st = stam_dmg * (1.0f - a->wep_l->stability / 100.0f) + 4.0f;   /* [версия] */
        a->sp -= st;
        g->blocks++;
        asy_log(g, "%s: BLOCKED with the shield, %.1f dmg, stamina -%.1f -> %.0f", what, dmg, st, a->sp);
        if (a->sp < 0) {
            a->sp = 0;
            ClipRef r = clip_name(&g->kcmb, "guard_break");
            if (clip_ok(r)) { body_play(&g->pb, r, 0.05f, 1); p->st = P_REACT; }
            asy_log(g, "%s: guard broken", what);
        } else {
            ClipRef r = clip_name(&g->kcmb, impact >= 2 ? "guard_hit_l" : "guard_hit");
            if (clip_ok(r)) { body_play(&g->pb, r, 0.05f, 1); p->st = P_REACT; }
        }
    } else {
        ClipRef r = clip_name(&g->kcmb, impact >= 3 ? "knockdown" : impact >= 2 ? "hit_l" : "hit_m");
        if (clip_ok(r)) { body_play(&g->pb, r, 0.05f, 1); p->st = P_REACT; p->yaw = face; }
        asy_log(g, "%s: player hit, %.1f dmg (hp %.0f -> %.0f)", what, dmg, a->hp, a->hp - dmg);
    }
    g->hits_taken++;
    a->hp -= dmg;
    prg_interrupt(&g->prg);
    if (a->hp <= 0) player_die(g, what);
}

static void fx_push(Game *g, int kind, const float pos[3], float yaw)
{
    if (g->n_fx >= 8) return;
    memcpy(g->fx_pos[g->n_fx], pos, 12);
    g->fx_dir[g->n_fx][0] = -sinf(yaw); g->fx_dir[g->n_fx][1] = 0.3f; g->fx_dir[g->n_fx][2] = -cosf(yaw);
    g->fx_kind[g->n_fx++] = kind;
}

/* ------------------------------------------------------------------ the knight */

static ClipRef kclip(Game *g, PSt st, int dir) { return g->kclip[st][dir]; }

static void player_set(Game *g, PSt st, int dir)
{
    Player *p = &g->pl;
    p->st = st;
    p->dir = dir;
    ClipRef r = kclip(g, st, dir);
    if (!clip_ok(r)) r = kclip(g, st, DIR_F);
    body_play(&g->pb, r, BLEND, st >= P_ROLL);
}

/* world move direction from the stick: basis = camera yaw frozen at deflection start (the controls design notes §4.4) */
static float stick_world(Player *p, const AshInput *in, float cam_yaw, float *mx, float *mz)
{
    if (in->move_mag <= 0) { p->basis_valid = 0; *mx = *mz = 0; return 0; }
    float dir = atan2f(in->move_x, -in->move_y);
    if (!p->basis_valid || fabsf(asy_wrap_pi(dir - p->basis_dir)) > 45.0f * ASY_PI / 180.0f) {
        p->basis_valid = 1;
        p->basis_yaw = cam_yaw;
        p->basis_dir = dir;
    }
    float s = sinf(p->basis_yaw), c = cosf(p->basis_yaw);
    float n = sqrtf(in->move_x * in->move_x + in->move_y * in->move_y);
    float fx = -in->move_y / n, rx = in->move_x / n;
    *mx = -s * fx + c * rx;
    *mz = -c * fx - s * rx;
    return in->move_mag;
}

static int dir_of(float local)
{
    float a = fabsf(local);
    if (a <= ASY_PI * 0.25f) return DIR_F;
    if (a >= ASY_PI * 0.75f) return DIR_B;
    return local > 0 ? DIR_L : DIR_R;
}

static int lock_target_pos(Game *g, int t, float out[3]);

/* starts a player action through the combat module (stamina). 0 = not allowed (no stamina). */
static int player_action(Game *g, PSt st, ClipRef r)
{
    if (!clip_ok(r)) return 0;
    u32 id = clip_anim_id(r);
    if (!cb_begin_anim(&g->cbw, &g->pact, id)) {
        if (g->verbose) asy_log(g, "action %u refused: stamina %.0f", (unsigned)id, g->pact.sp);
        return 0;
    }
    g->pl.st = st;
    body_play(&g->pb, r, 0.1f, 1);
    if (st == P_ROLL || st == P_BACKSTEP) fx_push(g, 3, g->pl.pos, g->pl.yaw);
    if (g->verbose && g->lock == 100) {
        float c = asy_flat_dist(g->pl.pos, g->B.pos);
        float rx = g->pl.pos[0] - g->B.pos[0], rz = g->pl.pos[2] - g->B.pos[2];
        float fr = (rx * -sinf(g->B.yaw) + rz * -cosf(g->B.yaw)) / (c + 1e-4f);
        asy_log(g, "player action %u at %.2f m from the demon's centre (front %.2f), facing off %.0f deg", (unsigned)id, c,
                fr, asy_wrap_pi(g->pl.yaw - atan2f(-(g->B.pos[0] - g->pl.pos[0]), -(g->B.pos[2] - g->pl.pos[2]))) * 57.3f);
    }
    return 1;
}

static void player_tick(Game *g, float dt)
{
    Player *p = &g->pl;
    const AshInput *in = &g->in;
    CbActor *a = &g->pact;

    /* ---- forced: reactions from combat (hit, guard, parried, riposte/backstab victim or attacker) */
    if (a->req_anim) {
        u32 id = a->req_anim;
        a->req_anim = 0;
        ClipRef r = clip_id(&g->kcmb, id);
        if (!clip_ok(r)) r = clip_id(&g->kbase, id);
        if (clip_ok(r) && !p->dead) {
            body_play(&g->pb, r, 0.05f, 1);
            p->st = (id == 233201 || id == 233400) ? P_THROW : P_REACT;
            if (id == 6000) { g->pb.hold = 1; p->st = P_PRG; }
            prg_interrupt(&g->prg);
        } else if (!clip_ok(r)) {
            asy_log(g, "player reaction %u: no clip", (unsigned)id);
        }
    }

    int locked = g->lock >= 0;
    float target[3] = {0, 0, 0};
    if (locked && !lock_target_pos(g, g->lock, target)) { g->lock = -1; locked = 0; }

    float mx = 0, mz = 0;
    float cam_yaw = g->cam.yaw;
    float to_t = locked ? yaw_to(p->pos, target) : 0;
    if (locked) { p->basis_valid = 0; cam_yaw = to_t; }
    float mag = 0;
    int input_ok = !p->dead && !prg_locked(&g->prg) && p->st != P_FOG && p->st != P_LADDER;
    if (input_ok) mag = stick_world(p, in, cam_yaw, &mx, &mz);
    float want_yaw = mag > 0 ? atan2f(-mx, -mz) : p->yaw;
    PSt loco = mag <= 0 ? P_IDLE : (in->held & ACT_RUN) ? P_RUN : in->jog ? P_JOG : P_WALK;
    float face = locked ? to_t : want_yaw;
    if (locked && loco == P_RUN) face = want_yaw;
    int ldir = (locked && (loco == P_WALK || loco == P_JOG)) ? dir_of(asy_wrap_pi(want_yaw - face)) : DIR_F;

    /* ---- buffered input (0.25 s, the controls design notes §4.1) */
    if (input_ok) {
        u32 req = 0;
        if (in->pressed & ACT_DODGE) req = ACT_DODGE;
        else if (in->pressed & ACT_PARRY) req = ACT_PARRY;
        else if (in->pressed & ACT_HEAVY) req = (in->held & ACT_GUARD) ? ACT_PARRY : ACT_HEAVY;
        else if (in->pressed & ACT_LIGHT) req = ACT_LIGHT;
        if (req) { p->buffered = (int)req; p->buffer_left = BUFFER_TIME; }
        else if (p->buffer_left > 0 && (p->buffer_left -= dt) <= 0) p->buffered = 0;
    }

    /* ---- progression owns the body (drink, rest, death) */
    if (prg_locked(&g->prg) || p->dead) {
        if (p->st != P_PRG && p->st != P_REACT) p->st = P_PRG;
    } else if (p->st == P_PRG) {
        player_set(g, P_IDLE, DIR_F);
    }

    int busy = p->st >= P_ROLL;
    if (p->st == P_FALL || p->st == P_PLUNGE) busy = 1;
    if (busy && p->st != P_GUARD && p->st != P_PRG && p->st != P_FOG && p->st != P_LADDER && p->st != P_FALL && p->st != P_PLUNGE) {
        int end = body_done(&g->pb, BLEND);
        /* combo: next R1 inside the attack's cancel window (TAE ChrActionFlag, src/combat flags) */
        int cancel_rh = (a->flags & (CB_ST_CANCEL_RH | CB_ST_CANCEL_ANY)) != 0;
        int cancel_roll = (a->flags & (CB_ST_CANCEL_ROLL | CB_ST_CANCEL_ANY)) != 0;
        if (p->st == P_ATTACK && p->buffered == ACT_LIGHT && cancel_rh && g->pb.t > 0.3f) end = 1;
        if ((p->st == P_ATTACK || p->st == P_REACT) && p->buffered == ACT_DODGE && cancel_roll && g->pb.t > 0.2f) end = 1;
        /* rolling R1 (a23_3900): R1 inside the roll's right-hand cancel window (src/combat 6.4) */
        if (p->st == P_ROLL && p->buffered == ACT_LIGHT && cancel_rh && clip_ok(g->k_roll_r1)) end = 1;
        if (end) {
            busy = 0;
            if (p->st != P_ATTACK) p->combo = 0;
            if (!p->buffered) { p->redirect = ldir != DIR_F; player_set(g, loco, ldir); }
        }
    }
    if (p->st == P_GUARD) busy = (in->held & ACT_GUARD) && !p->buffered;   /* shield up while R is held */
    if (p->st == P_FOG) {
        p->fog_left -= dt;
        if (p->fog_left <= 0) { busy = 0; player_set(g, P_IDLE, DIR_F); }
    }
    if (p->st == P_LADDER) {
        p->fog_left -= dt;
        if (p->fog_left <= 0) { busy = 0; player_set(g, P_IDLE, DIR_F); }
    }

    if (!busy && input_ok) {
        if (p->buffered) {
            int b = p->buffered;
            p->buffered = 0;
            p->buffer_left = 0;
            p->dir = DIR_F;
            p->redirect = 0;
            if (b == ACT_DODGE) {
                if (mag > 0) {
                    p->move_dir = want_yaw;
                    float local = locked ? asy_wrap_pi(want_yaw - face) : 0;
                    int d = DIR_F;
                    if (!locked || fabsf(local) <= ASY_PI * 0.375f) p->yaw = want_yaw;
                    else { p->yaw = face; d = fabsf(local) >= ASY_PI * 0.875f ? DIR_B : local > 0 ? DIR_L : DIR_R;
                           p->redirect = 1; }
                    ClipRef r = kclip(g, P_ROLL, d);
                    if (player_action(g, P_ROLL, r)) p->dir = d;
                } else {
                    player_action(g, P_BACKSTEP, kclip(g, P_BACKSTEP, DIR_F));
                }
            } else if (b == ACT_LIGHT) {
                if (locked) p->yaw = face;
                else if (mag > 0) p->yaw = want_yaw;
                int victim = cb_try_throw(&g->cbw, g->pidx);
                if (victim >= 0) {
                    asy_log(g, "R1: %s on actor %d", g->pact.req_anim == 233201 ? "RIPOSTE" : "BACKSTAB", victim);
                    if (g->pact.req_anim == 233201) g->ripostes++;
                    /* the reaction branch above starts the throw clip next step */
                } else if (p->st == P_RUN && clip_ok(g->k_run_r1)) {   /* running R1 a23_3500 (src/combat 6.4) */
                    if (player_action(g, P_ATTACK, g->k_run_r1)) { p->combo = 0; asy_log(g, "running attack"); }
                } else if (p->st == P_ROLL && clip_ok(g->k_roll_r1)) {   /* rolling R1 a23_3900 */
                    if (player_action(g, P_ATTACK, g->k_roll_r1)) { p->combo = 0; asy_log(g, "rolling attack"); }
                } else {
                    int k = (p->st == P_ATTACK) ? (p->combo + 1) % 3 : 0;
                    if (!clip_ok(g->k_attack[k])) k = 0;
                    if (player_action(g, P_ATTACK, g->k_attack[k])) p->combo = k;
                }
            } else if (b == ACT_HEAVY) {
                if (locked) p->yaw = face;
                if (p->st == P_RUN && clip_ok(g->k_run_r2)) {   /* run + R2: jump attack a23_3600 */
                    if (player_action(g, P_ATTACK, g->k_run_r2)) asy_log(g, "jump attack");
                } else
                player_action(g, P_ATTACK, g->k_heavy);
                p->combo = 0;
            } else if (b == ACT_PARRY) {
                if (locked) p->yaw = face;
                if (player_action(g, P_PARRY, g->k_parry)) asy_log(g, "parry swing");
            }
        } else if ((in->held & ACT_GUARD) && clip_ok(g->k_guard)) {
            if (p->st != P_GUARD) { p->st = P_GUARD; body_play(&g->pb, g->k_guard, 0.1f, 0); }
            if (locked) p->yaw = turn_toward(p->yaw, face, 6.0f * dt);
            else if (mag > 0) p->yaw = turn_toward(p->yaw, want_yaw, 6.0f * dt);
            p->move_dir = want_yaw;
            p->guard_walk = mag > 0 ? 1.4f * (mag > 0.6f ? 1.0f : 0.6f) : 0;   /* [урезание] no guard-walk clips: slide */
        } else {
            if (loco != p->st || ldir != p->dir) player_set(g, loco, ldir);
            p->redirect = ldir != DIR_F;
            p->move_dir = want_yaw;
            if (mag > 0 || locked) p->yaw = turn_toward(p->yaw, face, 12.0f * dt);
        }
    }
    p->move_heading = mag > 0 ? want_yaw : p->yaw;
    p->speed_frac = p->st == P_RUN ? 1.0f : p->st == P_JOG ? 0.75f : p->st == P_WALK ? 0.4f : 0.0f;

    /* ---- motion: root motion of the clip (or the fog walk), collision, objects, events */
    float d[4];
    body_root(&g->pb, dt, d);
    if (a->hitstop > 0) d[0] = d[1] = d[2] = d[3] = 0;
    float s = sinf(p->yaw), c = cosf(p->yaw);
    float wx = c * d[0] + s * d[2], wz = -s * d[0] + c * d[2];
    if (p->redirect && (p->st == P_WALK || p->st == P_JOG || p->st == P_ROLL)) {
        float len = sqrtf(wx * wx + wz * wz);
        wx = -sinf(p->move_dir) * len;
        wz = -cosf(p->move_dir) * len;
    }
    if (p->st == P_FOG) { wx = -s * (2.0f / 1.5f) * dt; wz = -c * (2.0f / 1.5f) * dt; }   /* 7410: 2 m in 1.5 s */
    if (p->st == P_GUARD && p->guard_walk > 0 && mag > 0) {
        wx = -sinf(p->move_dir) * p->guard_walk * dt;
        wz = -cosf(p->move_dir) * p->guard_walk * dt;
    }
    if (p->st == P_FALL || p->st == P_PLUNGE) { wx = p->carry[0]; wz = p->carry[2]; }   /* keep the momentum */
    else if (p->grounded) { p->carry[0] = wx; p->carry[2] = wz; }
    if (a->push_t > 0) { wx += a->push[0] * dt; wz += a->push[2] * dt; a->push_t -= dt; }
    int was_grounded = p->grounded;
    float prev[3] = {p->pos[0], p->pos[1], p->pos[2]};   /* side of thin door leaves before this step */
    if (g->has_col) {
        p->grounded = pcol_move(&g->C, &CAP, p->pos, wx, wz, &p->vy, dt, &p->floor_tri);
    } else {
        p->pos[0] += wx; p->pos[2] += wz; p->grounded = 1;
    }
    if (p->st != P_FOG) {
        int nw = world_obj_push_from(&g->W, p->pos, prev, CAP.radius, CAP.height);
        int ne = g->has_ev ? ev_push_out(&g->ev, p->pos, CAP.radius, 0) : 0;
        if (g->verbose && (nw || ne) && (g->frame % 30) == 0) {
            for (int i = 0; i < (int)g->ev.n_ent; i++) {
                EvObjInfo o;
                if (!ev_obj_get(&g->ev, i, &o) || asy_flat_dist(o.pos, p->pos) > 4.0f) continue;
                int wi = g->ev_obj_world[i];
                asy_log(g, "  near obj %u model %u en %d open %d destr %d world %d kind %d on %d", (unsigned)o.id,
                        (unsigned)o.model, o.enabled, o.open, o.destroyed, wi, wi >= 0 ? g->W.obj[wi].kind : -1,
                        wi >= 0 ? g->W.obj_on[wi] : -1);
            }
        }
        if (g->verbose && (nw || ne) && (g->frame % 30) == 0)
            asy_log(g, "blocked: world objects %d, event blockers %d at (%.2f %.2f %.2f)", nw, ne, p->pos[0], p->pos[1], p->pos[2]);
    }
    /* bodies: hollows and the demon are cylinders the knight cannot walk through [версия: radii from NpcParam] */
    for (int i = 0; i < g->n_en; i++) {
        Enemy *e = &g->en[i];
        if (!e->enabled || e->dead || fabsf(e->pos[1] - p->pos[1]) > 1.5f) continue;
        float dx = p->pos[0] - e->pos[0], dz = p->pos[2] - e->pos[2], r = 0.4f + CAP.radius, l = sqrtf(dx * dx + dz * dz);
        if (l < r && l > 1e-3f) { p->pos[0] = e->pos[0] + dx / l * r; p->pos[2] = e->pos[2] + dz / l * r; }
    }
    if (g->has_boss && g->boss_enabled && !g->boss_hidden && g->B.st != BS_INACTIVE && g->B.st != BS_DEAD &&
        p->pos[1] < g->B.pos[1] + 4.0f) {
        float dx = p->pos[0] - g->B.pos[0], dz = p->pos[2] - g->B.pos[2];
        /* [версия] body (push) radius 1.7 m, not NpcParam HitRadius 2.5: with 2.5 the knight could never reach the
           leg hurt capsules (combat data: legs at +-1.5 m from its centre) */
        float r = ASY_DEMON_BODY_R + CAP.radius, l = sqrtf(dx * dx + dz * dz);
        if (l < r && l > 1e-3f) { p->pos[0] = g->B.pos[0] + dx / l * r; p->pos[2] = g->B.pos[2] + dz / l * r; }
    }
    /* the body pushes above can shove the knight into a closed door (the Demon pins him against the back door
       1811110): resolve the objects again, on the side he came from */
    if (p->st != P_FOG && world_obj_push_from(&g->W, p->pos, prev, CAP.radius, CAP.height) && g->has_ev)
        ev_push_out(&g->ev, p->pos, CAP.radius, 0);
    if (p->st != P_THROW) p->yaw = asy_wrap_pi(p->yaw + d[3]);

    /* ---- falling and the plunge attack (DS1: R1 while falling onto an enemy) */
    if (!p->grounded) {
        if (was_grounded) p->fall_from = p->pos[1];
        if (p->fall_from - p->pos[1] > 1.0f && p->st != P_FALL && p->st != P_PLUNGE && p->st != P_PRG && !p->dead) {
            p->st = P_FALL;
            asy_log(g, "falling from y=%.2f", p->fall_from);
        }
        if (p->st == P_FALL && (in->pressed & ACT_LIGHT) && (clip_ok(g->k_plunge[0]) || clip_ok(g->k_heavy))) {
            /* plunge through the combat module (6.4): a23_3800 -> 3801 (loop) -> 3810; its AttackType 62 window
               hits once per plunge; without the clips: the heavy swing held up (older cook) */
            p->st = P_PLUNGE;
            g->plunge_done = 0;
            if (clip_ok(g->k_plunge[0])) {
                cb_begin_anim(&g->cbw, &g->pact, cb_attack_anim(&g->cbw, &g->pact, CB_ACT_PLUNGE, 0));
                body_play(&g->pb, g->k_plunge[0], 0.05f, 1);
            } else {
                body_play(&g->pb, g->k_heavy, 0.05f, 1);
            }
            asy_log(g, "PLUNGE ATTACK started (y=%.2f)", p->pos[1]);
        }
        if (p->st == P_PLUNGE) {
            if (!clip_ok(g->k_plunge[0])) { if (g->pb.t > 0.6f) g->pb.t = 0.6f; }   /* hold the raised sword */
            else if (clip_same(g->pb.cur, g->k_plunge[0]) && body_done(&g->pb, 0) && clip_ok(g->k_plunge[1])) {
                cb_begin_anim(&g->cbw, &g->pact, cb_attack_anim(&g->cbw, &g->pact, CB_ACT_PLUNGE_FALL, 0));
                body_play(&g->pb, g->k_plunge[1], 0.05f, 1);
            }
        }
    } else {
        p->safe_pos[0] = p->pos[0]; p->safe_pos[1] = p->pos[1]; p->safe_pos[2] = p->pos[2];
        if (p->st == P_FALL || p->st == P_PLUNGE) {
            float h = p->fall_from - p->pos[1];
            asy_log(g, "landed after %.1f m", h);
            int was_plunge = p->st == P_PLUNGE;
            if (was_plunge && clip_ok(g->k_plunge[2])) {   /* landing a23_3810 (charges its stamina) */
                cb_begin_anim(&g->cbw, &g->pact, cb_attack_anim(&g->cbw, &g->pact, CB_ACT_PLUNGE_LAND, 0));
                body_play(&g->pb, g->k_plunge[2], 0.05f, 1);
                p->st = P_ATTACK;
            }
            if (was_plunge && g->plunge_done) {
                asy_log(g, "plunge landed (hit already dealt by the combat module)");
            } else if (was_plunge && g->has_boss && boss_alive(&g->B) && g->B.st != BS_INACTIVE &&
                asy_flat_dist(p->pos, g->B.pos) < g->B.hdr->radius + 2.0f) {
                /* [версия] plunge = 4 x the weapon's AR against the demon's defence (DS1 plunge multiplier is per
                   weapon; the real AtkParam row is not cooked) */
                float dmg = cb_damage_formula(g->pact.wep_r ? g->pact.wep_r->ar[0] * 4.0f : 340.0f, g->bact.def[0]);
                int hp0 = g->B.hp, inv = boss_iframes(&g->B), bst = g->B.st, bez = g->B.ez;
                float bt = g->B.t;
                boss_take_hit(&g->B, (int)(dmg + 0.5f), 100, BOSS_HIT_PLUNGE);
                g->bact.hp = (float)g->B.hp;
                if (g->B.hp < hp0) {   /* counted only when the demon really lost HP */
                    if (g->boss_lua) boss_ai_interrupt(&g->BA, BOSS_AI_INT_DAMAGED);
                    g->plunges++;
                    asy_log(g, "PLUNGE HIT on the Asylum Demon (landing check): %.0f dmg, demon hp %d/%d", dmg,
                            g->B.hp, g->B.hp_max);
                } else {
                    asy_log(g, "PLUNGE NO DAMAGE on the Asylum Demon (landing check): demon st %d ez %d t %.2f "
                            "iframes %d, hp %d/%d", bst, bez, bt, inv, g->B.hp, g->B.hp_max);
                }
                g->plunge_done = 1;
                if (!clip_ok(g->k_plunge[2])) g->pb.t = 0.6f;   /* play the rest of the swing */
                p->st = P_ATTACK;
            } else if (was_plunge && clip_ok(g->k_plunge[2])) {
                if (h > 14.0f) player_die(g, "fall");
            } else {
                if (h > 14.0f) player_die(g, "fall");
                else player_set(g, P_IDLE, DIR_F);
            }
        }
    }
    if (p->pos[1] < g->void_y && !p->dead) player_die(g, "fell into the void");   /* m18_01: below every floor (150) */
    if (a->hitstop <= 0) body_advance(&g->pb, dt);
    else a->hitstop -= dt;
}

/* ------------------------------------------------------------------ lock-on (the controls design notes §4.6) */

static int lock_target_pos(Game *g, int t, float out[3])
{
    if (t == 100) {
        if (!g->has_boss || !boss_alive(&g->B) || g->B.st == BS_INACTIVE || g->boss_hidden || !g->boss_enabled) return 0;
        boss_lock_point(&g->B, out);
        out[1] = g->B.pos[1] + 0.0f;   /* strafing is planar; the camera gets the real point */
        return 1;
    }
    if (t < 0 || t >= g->n_en) return 0;
    Enemy *e = &g->en[t];
    if (!e->enabled || e->dead || e->dying) return 0;
    memcpy(out, e->pos, 12);
    return 1;
}

static void lock_point(Game *g, int t, float out[3])
{
    if (t == 100) { boss_lock_point(&g->B, out); return; }
    memcpy(out, g->en[t].pos, 12);
    out[1] += 1.25f;
}

static int lock_acquire(Game *g)
{
    Player *p = &g->pl;
    float fx = -sinf(g->cam.yaw), fz = -cosf(g->cam.yaw);
    float best = 1e9f;
    int bi = -1;
    for (int t = -1; t < g->n_en; t++) {
        int id = t < 0 ? 100 : t;
        float pos[3];
        if (!lock_target_pos(g, id, pos)) continue;
        if (fabsf(pos[1] - p->pos[1]) > 6.0f && id != 100) continue;
        float dist = asy_flat_dist(p->pos, pos);
        if (dist > (id == 100 ? 20.0f : g->row->lock_radius)) continue;
        float tx = pos[0] - p->pos[0], tz = pos[2] - p->pos[2], tl = sqrtf(tx * tx + tz * tz);
        float ang = tl > 1e-3f ? acosf(fminf(1.0f, fmaxf(-1.0f, (fx * tx + fz * tz) / tl))) : 0;
        if (ang > 50.0f * ASY_PI / 180 && dist > 6.0f) continue;
        float lp[3];
        lock_point(g, id, lp);
        if (g->has_col) {
            float d[3] = {lp[0] - g->cam.eye[0], lp[1] - g->cam.eye[1], lp[2] - g->cam.eye[2]}, tt;
            if (pcol_raycast(&g->C, g->cam.eye, d, &tt, -2.0f) >= 0 && tt < 0.97f) continue;
        }
        float score = dist + ang * 6.0f;
        if (score < best) { best = score; bi = id; }
    }
    return bi;
}

/* ------------------------------------------------------------------ hollows (c2500 on the 250000 Lua AI) */

static Enemy *en_of(AiAgent *a) { return (Enemy *)a->user; }

static float en_target(Enemy *e, int t, float out[3])
{
    if (t == 0 || t == 21) { memcpy(out, G->pl.pos, 12); return 1; }
    if (t == -1) { memcpy(out, e->pos, 12); return 1; }
    if (t == 100) { memcpy(out, e->home, 12); return 1; }
    return 0;
}
static float en_dist(AiAgent *a, int t)
{
    Enemy *e = en_of(a);
    float p[3];
    if (t == -1) return 0;
    if (!en_target(e, t, p)) return 9999;
    float d = asy_flat_dist(p, e->pos);
    if (t == 0) d -= CAP.radius + 0.4f;   /* surface distance [версия], as the demon's bridge */
    return d < 0 ? 0 : d;
}
static float en_angle(AiAgent *a, int t)
{
    Enemy *e = en_of(a);
    float p[3];
    if (!en_target(e, t, p)) return 0;
    return -asy_wrap_pi(yaw_to(e->pos, p) - e->yaw) * 180.0f / ASY_PI;
}
static void en_move(AiAgent *a, float fwd, float side, int walk, int face, float dt)
{
    (void)dt;
    Enemy *e = en_of(a);
    e->mv_fwd = fwd; e->mv_side = side; e->mv_walk = walk; e->mv_face = face; e->mv_set = 1;
}
static float en_ez(AiAgent *a, int ez)
{
    Enemy *e = en_of(a);
    Game *g = G;
    int clip = -1;
    if (ez >= 3000 && ez < 3100) clip = (ez == 3004 || ez == 3005 || ez == 3010) ? g->h_atk2 : g->h_atk1;
    if (clip < 0) return 0.4f;   /* steps 700-703 etc.: no clip cooked for c2500 [урезание] */
    ClipRef r = {&g->hanim, clip};
    body_play(&e->body, r, 0.1f, 1);
    e->ez = ez;
    e->ez_left = clip_dur(r);
    return e->ez_left;
}
static float en_dist_y(AiAgent *a, int t)
{
    Enemy *e = en_of(a);
    float p[3];
    return en_target(e, t, p) ? p[1] - e->pos[1] : 0;
}
static void en_log(AiAgent *a, const char *s)
{
    if (G->verbose > 1) asy_log(G, "hollow %u ai: %s", (unsigned)en_of(a)->entity, s);
}
static const AiWorld EN_WORLD = {en_dist, en_angle, en_move, en_ez, en_log, NULL, NULL, en_dist_y, NULL};

static int load_excel(Game *g, AiAgent *a, AsyLoadFn load, const char *path)
{
    (void)g;
    u8 *buf = NULL;
    int n = load(path, &buf);
    if (n <= 0) return -1;
    char *s = (char *)buf;
    s[n - 1] = s[n - 1] == '\n' ? 0 : s[n - 1];
    while (s && *s) {
        int k;
        float v;
        if (sscanf(s, "excel.%d=%f", &k, &v) == 2 && k >= 0 && k < AI_MAX_EXCEL) a->excel[k] = v;
        s = strchr(s, '\n');
        if (s) s++;
    }
    free(buf);
    return 0;
}

static double g_excel_hollow[AI_MAX_EXCEL];
static int g_excel_ok;

static void en_ai_init(Game *g, Enemy *e, int idx)
{
    if (!g->ai) return;
    if (e->ai_ok) ai_agent_free(g->ai, &e->ai);
    memset(&e->ai, 0, sizeof e->ai);
    e->ai_ok = ai_agent_init(g->ai, &e->ai, &EN_WORLD, "common10000_Logic", "common10000_Interupt",
                             0x2500u + 7919u * (u32)idx) == 0;
    if (e->ai_ok) {
        e->ai.user = e;
        memcpy(e->ai.excel, g_excel_hollow, sizeof g_excel_hollow);
        e->ai.battle_state = 0;
    }
}

static void en_reset(Game *g, Enemy *e, int idx)
{
    memcpy(e->pos, e->home, 12);
    e->yaw = e->home_yaw;
    e->vy = 0;
    e->dead = e->dying = 0;
    e->alerted = 0;
    e->ez = -1;
    e->ez_left = 0;
    e->react_t = 0;
    e->dead_t = 0;
    e->floor_tri = -1;
    e->body.hold = 0;
    body_play(&e->body, (ClipRef){&g->hanim, g->h_idle}, 0, 1);
    CbActor *a = &e->act;
    a->hp = a->hp_max;
    a->sp = a->sp_max;
    a->dead = 0;
    a->flags = 0;
    a->req_anim = 0;
    a->push_t = 0;
    a->hitstop = 0;
    a->throw_partner = -1;
    a->ripostable_t = 0;
    a->poise = a->poise_max;
    en_ai_init(g, e, idx);
}

static void en_tick(Game *g, Enemy *e, int idx, float dt)
{
    (void)idx;
    Player *p = &g->pl;
    CbActor *a = &e->act;
    a->team = (e->enabled && !e->dead) ? 1 : g->pact.team;
    if (!e->enabled) return;

    /* reactions from combat: the cooked clip with that anim id (hit 2000, parried 2100, riposted 2510,
       backstabbed 2500, death 2200), else hit / death */
    if (a->req_anim) {
        u32 id = a->req_anim;
        a->req_anim = 0;
        ClipRef r = clip_id(&g->hanim, id);
        if (!clip_ok(r) && g->has_hcmb) r = clip_id(&g->hcmb, id);   /* stagger 2004/2010, knockdown 2014, repel 2050 */
        if (!clip_ok(r)) r = (ClipRef){&g->hanim, a->dead ? g->h_death : g->h_hit};
        body_play(&e->body, r, 0.05f, 1);
        e->ez = -1;
        e->ez_left = 0;
        e->react_t = clip_dur(r);
        if (a->dead || id == 2510 || id == 2500) { e->dying = a->dead || e->dying; e->body.hold = 1; }
        if (a->dead) e->dying = 1;
    }
    if (a->dead && !e->dying) {
        e->dying = 1;
        body_play(&e->body, (ClipRef){&g->hanim, g->h_death}, 0.05f, 1);
        e->body.hold = 1;
    }
    if (e->dying) {
        e->dead_t += dt;
        if (!e->dead && e->dead_t > 0.5f && body_done(&e->body, 0) && e->act.throw_partner < 0) {
            e->dead = 1;
            asy_log(g, "hollow %u dead (death clip done)", (unsigned)e->entity);
        }
    }

    float d = asy_flat_dist(e->pos, p->pos), dy = p->pos[1] - e->pos[1];
    /* perception [версия]: sight 10 m in a 120 deg cone or 3 m anywhere, |dy| < 3 m, line of sight on the map */
    if (!e->alerted && !e->dying && !p->dead && e->ai_on) {
        float ang = fabsf(asy_wrap_pi(yaw_to(e->pos, p->pos) - e->yaw));
        if ((d < 10.0f && ang < ASY_PI / 3) || d < 3.0f || e->attacked) {
            int vis = fabsf(dy) < 3.0f;
            if (vis && g->has_col) {
                float o[3] = {e->pos[0], e->pos[1] + 1.4f, e->pos[2]};
                float v[3] = {p->pos[0] - o[0], p->pos[1] + 1.4f - o[1], p->pos[2] - o[2]}, t;
                if (pcol_raycast(&g->C, o, v, &t, -2.0f) >= 0 && t < 0.98f) vis = 0;
            }
            if (vis || e->attacked) {
                e->alerted = 1;
                if (e->ai_ok) { e->ai.battle_state = 1; e->ai.prev_battle_state = 0; }
                asy_log(g, "hollow %u sees the player (%.1f m)", (unsigned)e->entity, d);
            }
        }
    }
    if (e->alerted && (d > 22.0f || p->dead)) {
        e->alerted = 0;
        if (e->ai_ok) e->ai.battle_state = 0;
    }

    e->mv_set = 0;
    int reacting = e->react_t > 0;
    if (reacting) e->react_t -= dt;
    if (e->ez >= 0) {
        e->ez_left -= dt;
        if (e->ez_left <= 0) { e->ez = -1; body_play(&e->body, (ClipRef){&g->hanim, g->h_idle}, BLEND, 0); }
    }
    if (e->alerted && !e->dying && !reacting && e->ai_on && e->ai_ok && a->ripostable_t <= 0 && a->hitstop <= 0)
        ai_agent_tick(g->ai, &e->ai, dt);

    /* locomotion: AI move request, else idle; attacks / reactions: root motion of the clip */
    float wx = 0, wz = 0;
    if (!e->dying && !reacting && e->ez < 0) {
        if (e->mv_set && (e->mv_fwd != 0 || e->mv_side != 0)) {
            int clip = e->mv_walk ? g->h_walk : g->h_run;
            body_play(&e->body, (ClipRef){&g->hanim, clip}, BLEND, 0);
            float tgt[3];
            en_target(e, e->ai.move_target, tgt);
            float want = yaw_to(e->pos, tgt);
            float turn = (a->npc && a->npc->turn_vel > 0 ? a->npc->turn_vel : 180.0f) * ASY_PI / 180.0f * dt;
            if (e->mv_face) e->yaw = turn_toward(e->yaw, want, turn);
            float r4[4];
            body_root(&e->body, dt, r4);
            float speed = sqrtf(r4[0] * r4[0] + r4[2] * r4[2]);
            float fx = -sinf(e->yaw), fz = -cosf(e->yaw), rx = cosf(e->yaw), rz = -sinf(e->yaw);
            float mx = fx * e->mv_fwd + rx * e->mv_side, mz = fz * e->mv_fwd + rz * e->mv_side;
            float ml = sqrtf(mx * mx + mz * mz);
            if (ml > 1e-3f) { wx = mx / ml * speed; wz = mz / ml * speed; }
        } else {
            body_play(&e->body, (ClipRef){&g->hanim, g->h_idle}, BLEND, 0);
            if (e->alerted && e->mv_set && e->mv_face) {
                float turn = 180.0f * ASY_PI / 180.0f * dt;
                e->yaw = turn_toward(e->yaw, yaw_to(e->pos, p->pos), turn);
            }
        }
    } else if (!e->body.hold) {
        if (e->ez >= 0 && e->body.t < 0.35f) e->yaw = turn_toward(e->yaw, yaw_to(e->pos, p->pos), 4.0f * dt); /* [версия] */
        float r4[4];
        body_root(&e->body, dt, r4);
        float s = sinf(e->yaw), c = cosf(e->yaw);
        wx = c * r4[0] + s * r4[2];
        wz = -s * r4[0] + c * r4[2];
        if (e->dying) wx = wz = 0;
    }
    if (a->push_t > 0) { wx += a->push[0] * dt; wz += a->push[2] * dt; a->push_t -= dt; }
    if (g->has_col) {
        /* NPCs do not walk off ledges [версия: DS1 keeps them on their navmesh] */
        float h, ahead[3] = {e->pos[0] + wx * 6, e->pos[1], e->pos[2] + wz * 6};
        if ((wx != 0 || wz != 0) && !e->dying &&
            pcol_floor(&g->C, ahead[0], ahead[2], e->pos[1] + 0.6f, e->pos[1] - 1.2f, HCAP.slope_cos, &h) < 0)
            wx = wz = 0;
        pcol_move(&g->C, &HCAP, e->pos, wx, wz, &e->vy, dt, &e->floor_tri);
        ev_push_out(&g->ev, e->pos, 0.4f, 0);
    } else {
        e->pos[0] += wx; e->pos[2] += wz;
    }
    if (a->hitstop > 0) a->hitstop -= dt;
    else body_advance(&e->body, dt);
}

/* ------------------------------------------------------------------ the Asylum Demon */

static void boss_collide(void *ctx, float pos[3], float dx, float dz)
{
    static const PcolCapsule cap = {2.0f, 7.5f, 0.6f, 0.64f, 0.6f, 9.8f};   /* as src/boss/bosstest.c */
    int tri;
    pcol_move((Pcol *)ctx, &cap, pos, dx, dz, &G->boss_vy, ASY_DT, &tri);
}

static int boss_active(const Game *g)
{
    return g->has_boss && g->boss_enabled && !g->boss_hidden && g->B.st != BS_INACTIVE;
}

static void boss_step(Game *g, float dt)
{
    if (!g->has_boss) return;
    Boss *b = &g->B;
    BossWorld w = {{g->pl.pos[0], g->pl.pos[1], g->pl.pos[2]}, CAP.radius, g->pl.dead || !g->boss_ai_on,
                   g->has_col ? boss_collide : NULL, &g->C};
    if (!boss_active(g)) { g->bact.team = g->pact.team; return; }
    g->bact.team = 1;
    if (g->boss_lua && g->boss_ai_on) boss_ai_tick(&g->BA, &w, dt);
    else boss_tick(b, &w, dt);

    /* the demon's swings -> the player (boss_hits: AtkParam_Npc rows on the demon's own dummies) */
    BossHit hits[8];
    int n = boss_hits(b, hits, 8);
    Player *p = &g->pl;
    float pa[3] = {p->pos[0], p->pos[1] + 0.4f, p->pos[2]}, pb[3] = {p->pos[0], p->pos[1] + 1.5f, p->pos[2]};
    for (int i = 0; i < n && !p->dead; i++) {
        if (hits[i].swing == g->boss_last_swing) continue;
        float dd = cb_capsule_dist(hits[i].a, hits[i].b, pa, pb);
        if (dd > hits[i].radius + CAP.radius) continue;
        g->boss_last_swing = hits[i].swing;
        const BossFileAtk *atk = boss_atk(b, hits[i].atk);
        if (!atk) continue;
        float phys = atk->phys, fire = (atk->flags & BATK_BLAST) ? atk->phys * 0.5f : 0;   /* [версия] blast = fire */
        float mid[3] = {(hits[i].a[0] + hits[i].b[0]) * 0.5f, (hits[i].a[1] + hits[i].b[1]) * 0.5f,
                        (hits[i].a[2] + hits[i].b[2]) * 0.5f};
        int hb = g->hits_taken, bb = g->blocks;
        player_hit_external(g, phys, fire, atk->stam, atk->impact, b->pos, (atk->flags & BATK_IGNORE_GUARD) != 0,
                            "demon swing");
        if (g->hits_taken != hb || g->blocks != bb) fx_push(g, g->blocks != bb ? 2 : 1, mid, b->yaw);
    }
}

/* ------------------------------------------------------------------ events host */

static Enemy *en_find(Game *g, u32 id)
{
    for (int i = 0; i < g->n_en; i++) if (g->en[i].entity == id) return &g->en[i];
    return NULL;
}

static float q_query(void *u, int what, uint32_t id, uint32_t arg)
{
    Game *g = (Game *)u;
    (void)arg;
    if (id == 1810800 && g->has_boss) {
        if (what == EVQ_HP_RATIO) return g->B.hp_max ? (float)g->B.hp / g->B.hp_max : 1;
        if (what == EVQ_DEAD) return (float)boss_defeated(&g->B);
        return 0;
    }
    Enemy *e = en_find(g, id);
    if (!e) return what == EVQ_HP_RATIO ? 1.0f : 0.0f;
    if (what == EVQ_HP_RATIO) return e->act.hp_max > 0 ? e->act.hp / e->act.hp_max : 1;
    if (what == EVQ_DEAD) return (float)e->dead;
    if (what == EVQ_ATTACKED) return (float)e->attacked;
    return 0;
}

static int q_pos(void *u, uint32_t id, float pos[3], float *yaw)
{
    Game *g = (Game *)u;
    if (id == EV_PLAYER) { memcpy(pos, g->pl.pos, 12); if (yaw) *yaw = g->pl.yaw; return 1; }
    if (id == 1810800 && g->has_boss) { memcpy(pos, g->B.pos, 12); if (yaw) *yaw = g->B.yaw; return 1; }
    Enemy *e = en_find(g, id);
    if (!e) return 0;
    memcpy(pos, e->pos, 12);
    if (yaw) *yaw = e->yaw;
    return 1;
}

static void ev_fade(Game *g, float len)
{
    g->ev_fade_t = 0;
    g->ev_fade_len = len;
}

static void game_save(Game *g, const char *why)
{
    if (g->save_fn) { g->saves++; g->save_fn(g, why); return; }   /* full game: src/save (savedata A/B) */
    static u8 pblob[256], eblob[8192];
    int pn = prg_save_write(&g->prg, pblob, sizeof pblob);
    int en = ev_save_write(&g->ev, eblob, sizeof eblob);
    SavePart parts[2] = {{SAVE_TAG_PROGRESS, pblob, pn}, {SAVE_TAG_EVENTS, eblob, en}};
    int r = (pn > 0 && en > 0) ? save_write_file(SAVE_PATH, parts, 2) : -1;
    g->saves++;
    asy_log(g, "save (%s) %s -> %d: progress %d B, events %d B, souls %d", why, SAVE_PATH, r, pn, en, (int)g->prg.souls);
}

static void game_reload(Game *g);

static float q_cmd(void *u, const EvCmd *c)
{
    Game *g = (Game *)u;
    Player *p = &g->pl;
    switch (c->op) {
    case EV_CMD_LOG:
        if (g->verbose) asy_log(g, "ev log: %s", c->text ? c->text : "");
        return 0;
    case EV_CMD_CHR_SET: {
        if (c->id == 1810800) {
            if (c->a == EV_CHR_ENABLE) g->boss_enabled = c->b;
            if (c->a == EV_CHR_AI) {
                g->boss_ai_on = c->b;
                if (c->b && g->has_boss && (g->B.st == BS_WAIT)) { g->B.engaged = 1; g->B.st = BS_IDLE; }
            }
            if (g->verbose) asy_log(g, "demon set %d = %d", (int)c->a, (int)c->b);
            return 0;
        }
        Enemy *e = en_find(g, c->id);
        if (e && c->a == EV_CHR_ENABLE) e->enabled = c->b;
        if (e && c->a == EV_CHR_AI) e->ai_on = c->b;
        return 0;
    }
    case EV_CMD_KILL: {
        Enemy *e = en_find(g, c->id);
        if (e) { e->act.hp = 0; e->act.dead = 1; e->dying = 1; e->dead = 1; }
        return 0;
    }
    case EV_CMD_CHR_ANIM:
        if (c->id == EV_PLAYER) {
            if (c->a == 7410) {   /* walk through the fog wall */
                p->st = P_FOG;
                p->fog_left = 1.5f;
                body_play(&g->pb, g->k_walk, BLEND, 1);
                asy_log(g, "fog wall: walking through (7410)");
                return 1.5f;
            }
            return 0.5f;
        }
        if (c->id == 1810800 && g->has_boss) {
            if (c->a == 9060) {
                g->boss_hidden = 0;
                boss_enter_drop(&g->B);
                asy_log(g, "the Asylum Demon drops from the ceiling (9060)");
                return anim_duration(&g->B.anim, g->B.clip_entrance);
            }
            return 1.0f;
        }
        return 1.0f;
    case EV_CMD_MOVE:
        if (c->id == EV_PLAYER) {
            memcpy(p->pos, c->pos, 12);
            p->yaw = c->yaw;
            p->vy = 0;
            if (g->has_col) {
                float h;
                int t = pcol_floor(&g->C, p->pos[0], p->pos[2], p->pos[1] + 1.0f, p->pos[1] - 4.0f, CAP.slope_cos, &h);
                if (t >= 0) { p->pos[1] = h; p->floor_tri = t; }
            }
            cam_reset(&g->cam, &g->ccfg, g->row, p->pos, p->yaw);
            asy_log(g, "player warped to (%.2f %.2f %.2f)", p->pos[0], p->pos[1], p->pos[2]);
        } else if (c->id == 1810800 && g->has_boss) {
            int in_air = c->pos[1] > 205.0f;   /* 1812305: hanging above the hall before the drop */
            if (in_air) { g->B.st = BS_INACTIVE; g->boss_hidden = 1; memcpy(g->B.pos, c->pos, 12); g->B.yaw = c->yaw; }
            else if (g->B.st == BS_INACTIVE) { boss_reset(&g->B, c->pos, c->yaw); g->boss_hidden = 0; }
            else { memcpy(g->B.pos, c->pos, 12); g->B.yaw = c->yaw; g->boss_hidden = 0; }
            asy_log(g, "demon moved to (%.2f %.2f %.2f)", c->pos[0], c->pos[1], c->pos[2]);
        } else {
            Enemy *e = en_find(g, c->id);
            if (e) { memcpy(e->pos, c->pos, 12); e->yaw = c->yaw; }
        }
        return 0;
    case EV_CMD_FACE:
        if (c->id == EV_PLAYER) p->yaw = yaw_to(p->pos, c->pos);
        else if (c->id == 1810800 && g->has_boss) g->B.yaw = yaw_to(g->B.pos, c->pos);
        return 0;
    case EV_CMD_STANDBY:
        if (c->id == 1810800 && g->has_boss && c->a == 9000) {   /* hangs above the hall: not in the level yet */
            g->B.st = BS_INACTIVE;
            g->boss_hidden = 1;
        }
        return 0;
    case EV_CMD_BOSS_BAR:
        g->boss_bar = c->a;
        asy_log(g, "boss bar %s", c->a ? "ON" : "off");
        return 0;
    case EV_CMD_BOSS_START:
        return 0;
    case EV_CMD_KILL_BOSS:
        prg_banner(&g->prg, IMG_VICTORY);
        prg_add_souls(&g->prg, 2000);   /* [версия] DS1 wiki: 2000 souls; NpcParam SoulReward = 0, the event gives them */
        g->victory = 1;
        asy_log(g, "VICTORY ACHIEVED: the Asylum Demon is dead, +2000 souls -> %d", (int)g->prg.souls + 0);
        return 0;
    case EV_CMD_OBJ_ANIM:
        return c->f[0] < 0 ? 0 : 1.0f;
    case EV_CMD_DIALOG:
    case EV_CMD_BANNER:
        snprintf(g->toast_buf, sizeof g->toast_buf, "%s", c->text ? c->text : "");
        g->toast_text = g->toast_buf;
        g->toast_t = 0;
        asy_log(g, "message: %s", g->toast_buf);
        return 0;
    case EV_CMD_ITEM:
        if (g->has_items && g->class_row) {   /* src/items: the item goes into the inventory, FMG "Получено: ..." */
            int n = itm_add(&g->inv, &g->idb, (uint32_t)c->a, c->b > 0 ? (int)c->b : 1);
            ItmGot got = {(uint32_t)c->a, c->b > 0 ? (int)c->b : 1, n};
            if (itm_got_text(&g->idb, &got, 1, g->toast_buf, 96) <= 0)
                snprintf(g->toast_buf, sizeof g->toast_buf, "%s", c->text ? c->text : "?");
            g->toast_text = g->toast_buf;
            g->toast_t = 0;
            g->items++;
            asy_log(g, "item %08x x%d -> inventory (+%d, %d entries): %s", (unsigned)c->a, (int)c->b, n, g->inv.n,
                    g->toast_buf);
            game_apply_equipment(g);
            return 0;
        }
        if (c->b > 1) snprintf(g->toast_buf, sizeof g->toast_buf, "%s x%d", c->text ? c->text : "?", (int)c->b);
        else snprintf(g->toast_buf, sizeof g->toast_buf, "%s", c->text ? c->text : "?");
        g->toast_text = g->toast_buf;
        g->toast_t = 0;
        g->items++;
        asy_log(g, "item: %s", g->toast_buf);
        return 0;
    case EV_CMD_CUTSCENE:
        ev_fade(g, 2.0f);
        prg_banner(&g->prg, IMG_AREA_ASYLUM);
        asy_log(g, "cutscene %u -> fade + area title", (unsigned)c->id);
        return 2.0f;
    case EV_CMD_LADDER:
        ev_fade(g, 1.0f);
        memcpy(p->pos, c->pos, 12);
        p->yaw = c->yaw;
        p->vy = 0;
        p->st = P_LADDER;
        p->fog_left = 1.0f;
        body_play(&g->pb, kclip(g, P_IDLE, DIR_F), 0, 1);
        cam_reset(&g->cam, &g->ccfg, g->row, p->pos, p->yaw);
        asy_log(g, "ladder %s -> (%.2f %.2f %.2f)", c->a ? "up" : "down", p->pos[0], p->pos[1], p->pos[2]);
        return 1.0f;
    case EV_CMD_BONFIRE_LIT:
        prg_banner(&g->prg, IMG_BONFIRE_LIT);
        g->bonfires_lit++;
        asy_log(g, "BONFIRE LIT %u", (unsigned)c->id);
        return 0;
    case EV_CMD_REST:
        prg_rest_begin(&g->prg);
        g->rests++;
        asy_log(g, "rest at bonfire %u", (unsigned)c->id);
        return 2.0f;
    case EV_CMD_RELOAD:
        game_reload(g);
        return 0;
    case EV_CMD_SAVE:
        game_save(g, "event");
        return 0;
    case EV_CMD_BALL: {
        int oi = world_find_obj(&g->W, c->id);
        float len = c->f[0] > 0 ? sqrtf((c->pos2[0] - c->pos[0]) * (c->pos2[0] - c->pos[0]) +
                                        (c->pos2[2] - c->pos[2]) * (c->pos2[2] - c->pos[2])) / c->f[0] : 2.0f;
        g->ball.on = 1;
        g->ball.obj = oi;
        memcpy(g->ball.p0, c->pos, 12);
        memcpy(g->ball.p1, c->pos2, 12);
        g->ball.t = 0;
        g->ball.len = len;
        g->ball.radius = c->f[1] > 0 ? c->f[1] : 1.5f;
        g->ball.hit = 0;
        asy_log(g, "iron ball released (obj %d), %.1f s", oi, len);
        return len;
    }
    case EV_CMD_BOMB:
        for (int i = 0; i < ASY_MAX_BOMB; i++) {
            Bomb *b = &g->bomb[i];
            if (b->on) continue;
            b->on = 1;
            memcpy(b->p0, c->pos, 12);
            memcpy(b->p1, p->pos, 12);   /* thrown at where the player is now */
            b->t = 0;
            b->len = 1.0f;
            asy_log(g, "firebomb thrown");
            break;
        }
        return 0;
    case EV_CMD_END_DEMO:
        g->end_demo = 1;
        asy_log(g, "END OF DEMO: %s", c->text ? c->text : "");
        return 0;
    case EV_CMD_MAPCOL: {
        int part = g->has_col && c->text ? pcol_part_find(&g->C, c->text) : -1;
        if (part >= 0) pcol_part_enable(&g->C, part, c->a);
        asy_log(g, "map collision %u (%s) %s%s", (unsigned)c->id, c->text ? c->text : "?", c->a ? "ON" : "off",
                part < 0 ? " - part not in the zone collision" : "");
        return 0;
    }
    case EV_CMD_OBJACT:
        if (!c->b && c->text) {
            snprintf(g->toast_buf, sizeof g->toast_buf, "%s", c->text);
            g->toast_text = g->toast_buf;
            g->toast_t = 0;
        }
        return c->b ? 0.6f : 0;
    default:
        return 0;
    }
}

/* ------------------------------------------------------------------ reload (death / rest) */

static void game_reload(Game *g)
{
    Player *p = &g->pl;
    prg_on_reload(&g->prg);
    float pos[3], yaw;
    if (!g->has_ev) {   /* zone without events: its bonfire */
        memcpy(p->pos, g->respawn, 12);
        p->yaw = g->respawn[3];
    } else if (ev_respawn(&g->ev, pos, &yaw)) {
        memcpy(p->pos, pos, 12);
        p->yaw = yaw;
    }
    p->vy = 0;
    p->dead = 0;
    p->buffered = 0;
    if (g->has_col) {
        float h;
        int t = pcol_floor(&g->C, p->pos[0], p->pos[2], p->pos[1] + 1.5f, p->pos[1] - 4.0f, CAP.slope_cos, &h);
        if (t >= 0) { p->pos[1] = h; p->floor_tri = t; }
    }
    memcpy(p->safe_pos, p->pos, 12);
    CbActor *a = &g->pact;
    a->hp = a->hp_max;
    a->sp = a->sp_max;
    a->dead = 0;
    a->req_anim = 0;
    a->flags = 0;
    a->push_t = 0;
    a->hitstop = 0;
    a->throw_partner = -1;
    a->poise = a->poise_max;
    g->pb.hold = 0;
    if (!prg_locked(&g->prg)) player_set(g, P_IDLE, DIR_F);
    for (int i = 0; i < g->n_en; i++) en_reset(g, &g->en[i], i);
    if (g->has_boss) {
        if (g->boss_lua) ai_agent_free(g->ai, &g->BA.agent);
        boss_reset(&g->B, NULL, 0);
        if (g->boss_lua) boss_ai_attach(&g->BA, g->ai, &g->B, "ai/think_223200.txt", 0x2232u + g->reloads);
        g->bact.hp = (float)g->B.hp;
        g->bact.dead = 0;
        g->boss_ai_on = 0;
    }
    for (int i = 0; i < ASY_MAX_BOMB; i++) g->bomb[i].on = 0;
    g->lock = -1;
    g->boss_bar = 0;
    g->reloads++;
    cam_reset(&g->cam, &g->ccfg, g->row, p->pos, p->yaw);
    asy_log(g, "RELOAD #%d: player at (%.2f %.2f %.2f) hp %.0f estus %d souls %d stain %d", g->reloads, p->pos[0],
            p->pos[1], p->pos[2], a->hp, (int)g->prg.estus, (int)g->prg.souls, (int)g->prg.stain_on);
}

/* ------------------------------------------------------------------ loading */

static int load_into(AsyLoadFn load, const char *name, u8 **out, int need)
{
    int n = load(name, out);
    if (n <= 0) {
        *out = NULL;
        printf("ash-asy: %s %s\n", name, need ? "MISSING" : "missing (optional)");
    }
    return n;
}

static int lua_setup(Game *g, AsyLoadFn load)
{
    u8 *buf = NULL;
    int n = load("ai/aiCommon/load_order.txt", &buf);
    if (n <= 0) { printf("ash-asy: no ai/ -> hollows and the demon use no Lua (demon: C state machine)\n"); return 0; }
    buf[n - 1] = buf[n - 1] == '\n' ? 0 : buf[n - 1];
    g->ai = ai_host_new();
    int loaded = 0;
    char *s = (char *)buf, path[128];
    while (s && *s) {
        char *e = strpbrk(s, "\r\n");
        int len = e ? (int)(e - s) : (int)strlen(s);
        if (len > 0 && len < 64) {
            char nm[64];
            memcpy(nm, s, len);
            nm[len] = 0;
            int enemy = (len > 7 && !strcmp(nm + len - 7, "_battle")) || (len > 6 && !strcmp(nm + len - 6, "_logic"));
            if (!enemy || !strcmp(nm, "010000_logic")) {
                snprintf(path, sizeof path, "ai/aiCommon/%s.luac", nm);
                if (ai_host_load(g->ai, path)) { printf("ash-asy: lua load failed %s\n", path); return 0; }
                loaded++;
            }
        }
        s = e ? e + 1 : NULL;
        while (s && (*s == '\r' || *s == '\n')) s++;
    }
    free(buf);
    if (ai_host_load(g->ai, "ai/m18_01_00_00/223200_battle.luac")) { printf("ash-asy: no 223200_battle\n"); return 0; }
    if (ai_host_load(g->ai, "ai/m18_01_00_00/250000_battle.luac")) { printf("ash-asy: no 250000_battle\n"); return 0; }
    AiAgent tmp;
    memset(&tmp, 0, sizeof tmp);
    g_excel_ok = load_excel(g, &tmp, load, "ai/think_250022.txt") == 0;
    memcpy(g_excel_hollow, tmp.excel, sizeof g_excel_hollow);
    printf("ash-asy: Lua AI: %d aiCommon chunks + 223200_battle + 250000_battle, hollow think %s\n", loaded,
           g_excel_ok ? "ok" : "MISSING");
    return 1;
}

/* everything that stays across zones: combat data, the player body and clips, items, UI, progression */
int game_load_common(Game *g, AsyLoadFn load)
{
    G = g;
    u8 *f;
    int n;
    /* combat data */
    n = load_into(load, "combat.ashcb", &f, 1);
    if (n <= 0 || cb_data_load(&g->cbd, f, n)) { printf("ash-asy: combat.ashcb bad\n"); return -1; }
    cb_world_init(&g->cbw, &g->cbd);
    /* knight */
    n = load_into(load, "knight.ashc", &f, 1);
    if (n <= 0 || chr_load(&g->kchr, f, n)) return -1;
    g->f_body = f;
    n = load_into(load, "knight.ashanim", &f, 1);
    if (n <= 0 || anim_load(&g->kbase, f, n)) return -1;
    n = load_into(load, "knight_combat.ashanim", &f, 0);
    g->has_cmb = n > 0 && anim_load(&g->kcmb, f, n) == 0 && g->kcmb.n_bones == g->kbase.n_bones;
    n = load_into(load, "knight_prg.ashanim", &f, 0);
    g->has_prg = n > 0 && anim_load(&g->kprg, f, n) == 0 && g->kprg.n_bones == g->kbase.n_bones;
    static const char *const base_names[P_COUNT] = {"idle", "walk", "jog", "run", "roll", "backstep", "attack"};
    static const char *const sfx[DIR_COUNT] = {"", "_b", "_l", "_r"};
    for (int s = 0; s < P_COUNT; s++)
        for (int d = 0; d < DIR_COUNT; d++) {
            g->kclip[s][d] = (ClipRef){NULL, -1};
            if (s > P_ATTACK || !base_names[s]) continue;
            char nm[16];
            snprintf(nm, sizeof nm, "%s%s", base_names[s], sfx[d]);
            g->kclip[s][d] = clip_name(&g->kbase, nm);
            if (!clip_ok(g->kclip[s][d])) g->kclip[s][d] = g->kclip[s][DIR_F];
        }
    g->k_walk = g->kclip[P_WALK][DIR_F];
    g->k_attack[0] = clip_name(&g->kbase, "attack");
    g->k_attack[1] = clip_name(&g->kcmb, "attack2");
    g->k_attack[2] = clip_name(&g->kcmb, "attack3");
    g->k_heavy = clip_name(&g->kcmb, "heavy");
    g->k_guard = clip_name(&g->kcmb, "guard");
    g->k_parry = clip_name(&g->kcmb, "parry");
    g->k_death = clip_name(&g->kcmb, "death");
    g->k_plunge[0] = clip_name(&g->kcmb, "plunge");   /* src/combat 6.4 clips (absent in older cooks: fallbacks) */
    g->k_plunge[1] = clip_name(&g->kcmb, "plunge_fall");
    g->k_plunge[2] = clip_name(&g->kcmb, "plunge_land");
    g->k_run_r1 = clip_name(&g->kcmb, "run_attack");
    g->k_run_r2 = clip_name(&g->kcmb, "jump_attack");
    g->k_roll_r1 = clip_name(&g->kcmb, "roll_attack");
    printf("ash-asy: knight %u bones, base %u clips, combat %s, progress %s\n", (unsigned)g->kchr.hdr->n_bone,
           (unsigned)g->kbase.hdr->n_clips, g->has_cmb ? "ok" : "MISSING", g->has_prg ? "ok" : "MISSING");
    /* items (src/items, docs/items.md): optional; the inventory is used once a class is set (game_set_class) */
    n = load_into(load, "items.ashit", &f, 0);
    g->has_items = n > 0 && itm_db_load(&g->idb, f, n) == 0;
    g->f_items = n > 0 ? f : NULL;
    itm_inv_init(&g->inv);
    g->rng = 0x2545f491u;
    /* UI */
    n = load_into(load, "ui.ashui", &f, 0);
    g->ui_ok = n > 0 && ui_load(&g->ui, f, n) == 0;
    if (!g->ui_ok) memset(&g->ui, 0, sizeof g->ui);
    hud_init(&g->hud, &g->ui);
    /* combat actors */
    cb_actor_init_player(&g->pact, &g->cbd);
    cb_actor_bind(&g->pact, &g->cbd, bone_lookup, &g->kchr);
    g->pidx = cb_world_add(&g->cbw, &g->pact);
    g->pb.chr = &g->kchr;
    /* progression */
    prg_init(&g->prg, 0);
    prg_bind_hp(&g->prg, &g->pact.hp, &g->pact.hp_max);
    if (g->has_prg)
        for (int a = 1; a < PRG_ANIM_COUNT; a++) {
            int c = anim_find(&g->kprg, prg_anim_name[a]);
            if (c >= 0) prg_set_clip_len(&g->prg, a, anim_duration(&g->kprg, c));
        }
    return 0;
}

/* the Northern Undead Asylum m18_01: zone, hollows, Lua AI, the demon, the event script */
int game_zone_load_asylum(Game *g, AsyLoadFn load)
{
    G = g;
    u8 *f;
    int n;
    /* zone */
    n = load_into(load, "zone_m18_01.pak", &f, 1);
    if (n <= 0 || world_load(&g->W, f, n)) { printf("ash-asy: zone pak bad\n"); return -1; }
    g->f_pak = f;
    g->zone = ASY_ZONE_ASYLUM;
    g->void_y = 150.0f;
    g->has_col = g->W.pcol && pcol_load(&g->C, g->W.pcol, g->W.pcol_size) == 0;
    /* the gate's "closed" wall (MSB collision 1813121) is off until the event closes the gate */
    if (g->has_col) pcol_part_enable(&g->C, pcol_part_find(&g->C, "h0550B1_0000"), 0);
    printf("ash-asy: zone %d KB: %u batches, %u objs, %u chrs, %u regions; pcol %s %u tris\n", n / 1024,
           (unsigned)g->W.w->n_batch, (unsigned)g->W.z->n_obj, (unsigned)g->W.z->n_chr, (unsigned)g->W.z->n_reg,
           g->has_col ? "ok" : "MISSING", g->has_col ? (unsigned)g->C.hdr->n_tri : 0u);
    /* hollow */
    n = load_into(load, "hollow.ashc", &f, 0);
    g->f_hollow[0] = n > 0 ? f : NULL;
    if (n > 0 && chr_load(&g->hchr, f, n) == 0) {
        n = load_into(load, "hollow.ashanim", &f, 0);
        g->f_hollow[1] = n > 0 ? f : NULL;
        g->has_hollow = n > 0 && anim_load(&g->hanim, f, n) == 0;
    }
    if (g->has_hollow) {   /* stagger / knockdown / repelled clips of the combat module (6.4) */
        n = load_into(load, "hollow_combat.ashanim", &f, 0);
        g->f_hollow[2] = n > 0 ? f : NULL;
        g->has_hcmb = n > 0 && anim_load(&g->hcmb, f, n) == 0 && g->hcmb.n_bones == g->hanim.n_bones;
    }
    if (g->has_hollow) {
        g->h_idle = anim_find(&g->hanim, "idle"); g->h_walk = anim_find(&g->hanim, "walk");
        g->h_run = anim_find(&g->hanim, "run"); g->h_atk1 = anim_find(&g->hanim, "attack1");
        g->h_atk2 = anim_find(&g->hanim, "attack2"); g->h_hit = anim_find(&g->hanim, "hit");
        g->h_death = anim_find(&g->hanim, "death");
        if (g->h_atk2 < 0) g->h_atk2 = g->h_atk1;
    }
    /* AI (Lua) */
    int lua = lua_setup(g, load);
    /* boss */
    u8 *bc, *ba, *bb, *bn;
    int nc = load_into(load, "boss.ashc", &bc, 0), na = load_into(load, "boss.ashanim", &ba, 0);
    int nb = load_into(load, "boss.ashboss", &bb, 0), nn = load_into(load, "boss_name.ashtx", &bn, 0);
    g->f_boss[0] = nc > 0 ? bc : NULL; g->f_boss[1] = na > 0 ? ba : NULL;
    g->f_boss[2] = nb > 0 ? bb : NULL; g->f_boss[3] = nn > 0 ? bn : NULL;
    if (nc > 0 && na > 0 && nb > 0) {
        int rc = boss_load(&g->B, bc, nc, ba, na, bb, nb, nn > 0 ? bn : NULL, nn > 0 ? nn : 0);
        g->has_boss = rc == 0;
        if (!g->has_boss) printf("ash-asy: boss_load failed %d\n", rc);
    }
    if (g->has_boss) {
        boss_reset(&g->B, NULL, 0);
        if (lua) g->boss_lua = boss_ai_attach(&g->BA, g->ai, &g->B, "ai/think_223200.txt", 0x2232u) == 0;
        printf("ash-asy: Asylum Demon c2232: hp %d, AI %s\n", g->B.hp_max, g->boss_lua ? "Lua 223200_battle" : "C FSM");
    }
    /* events */
    n = load_into(load, "m18_01.ashevt", &f, 1);
    if (n <= 0) return -1;
    g->f_ev = f;
    g->has_ev = 1;
    int ms = ev_mem_size(f, n);
    g->ev_mem = calloc(1, ms > 0 ? ms : 16);
    EvHost host = {g, q_query, q_pos, q_cmd};
    if (ev_load(&g->ev, f, n, g->ev_mem, &host)) { printf("ash-asy: ev_load failed\n"); return -1; }
    g->ev_obj_world = (int *)malloc(sizeof(int) * (g->ev.n_ent + 1));
    int mapped = 0;
    for (int i = 0; i < (int)g->ev.n_ent; i++) {
        EvObjInfo o;
        g->ev_obj_world[i] = ev_obj_get(&g->ev, i, &o) ? world_find_obj(&g->W, o.id) : -1;
        if (g->ev_obj_world[i] >= 0) mapped++;
    }
    printf("ash-asy: events %d B (+%d B runtime), %u instances, %d objects mapped to the world\n", n, ms,
           (unsigned)g->ev.n_inst, mapped);

    /* hollows of the slice (World chr flag WC_CORE, model c2500) */
    for (u32 i = 0; i < g->W.z->n_chr && g->has_hollow && g->n_en < ASY_MAX_EN; i++) {
        const ZoneChr *zc = &g->W.chr[i];
        if (strncmp(zc->model, "c2500", 5) || !(zc->flags & WC_CORE)) continue;
        Enemy *e = &g->en[g->n_en];
        memset(e, 0, sizeof *e);
        e->entity = zc->entity;
        e->zchr = (int)i;
        e->body.chr = &g->hchr;
        memcpy(e->home, zc->pos, 12);
        e->home_yaw = zc->yaw;
        if (g->has_col) {
            float h;
            if (pcol_floor(&g->C, e->home[0], e->home[2], e->home[1] + 1.0f, e->home[1] - 3.0f, 0.5f, &h) >= 0) e->home[1] = h;
        }
        u32 npc = zc->npc_param;
        if (cb_actor_init_npc(&e->act, &g->cbd, npc, "c2500") < 0 && cb_actor_init_npc(&e->act, &g->cbd, 250022, "c2500") < 0) {
            printf("ash-asy: hollow %u: no NpcParam %u in combat data\n", (unsigned)e->entity, (unsigned)npc);
            continue;
        }
        cb_actor_bind(&e->act, &g->cbd, bone_lookup, &g->hchr);
        e->cbi = cb_world_add(&g->cbw, &e->act);
        e->enabled = 1;
        e->ai_on = 1;
        en_reset(g, e, g->n_en);
        printf("ash-asy: hollow %u npc %u hp %.0f at (%.2f %.2f %.2f)\n", (unsigned)e->entity, (unsigned)npc,
               e->act.hp_max, e->pos[0], e->pos[1], e->pos[2]);
        g->n_en++;
    }
    if (g->has_boss) {
        if (cb_actor_init_npc(&g->bact, &g->cbd, 223200, "c2232") == 0) {
            cb_actor_bind(&g->bact, &g->cbd, bone_lookup, &g->B.chr);
            g->bidx = cb_world_add(&g->cbw, &g->bact);
            g->bact.hp = g->bact.hp_max = (float)g->B.hp_max;
            const CbKind *k = &g->cbd.kind[g->bact.kind];
            int nh = k->n_hurt < CB_MAX_HURT ? k->n_hurt : CB_MAX_HURT, ok = 0;
            for (int i = 0; i < nh; i++) ok += g->bact.hurt_bone[i][0] >= 0 && g->bact.hurt_bone[i][1] >= 0;
            printf("ash-asy: demon as a combat body: %d hurt capsules, %d on animated bones (rest: bind pose)\n", nh, ok);
            for (int i = 0; i < nh; i++) {
                const CbHurt *h = &g->cbd.hurt[k->hurt_first + i];
                printf("ash-asy:   hurt %s-%s r %.2f a(%.2f %.2f %.2f) b(%.2f %.2f %.2f)\n", h->bone_a, h->bone_b, h->radius,
                       h->model_a[0], h->model_a[1], h->model_a[2], h->model_b[0], h->model_b[1], h->model_b[2]);
            }
        } else {
            printf("ash-asy: no c2232 in combat data\n");
            g->bidx = -1;
        }
    }

    printf("ash-asy: player hp %.0f stamina %.0f, weapon %u shield %u; %d hollows\n", g->pact.hp_max, g->pact.sp_max,
           g->pact.wep_r ? (unsigned)g->pact.wep_r->id : 0u, g->pact.wep_l ? (unsigned)g->pact.wep_l->id : 0u, g->n_en);
    return 0;
}

int game_load(Game *g, AsyLoadFn load)
{
    if (game_load_common(g, load) < 0) return -1;
    return game_zone_load_asylum(g, load);
}

void game_start(Game *g, int scenario)
{
    Player *p = &g->pl;
    memset(p, 0, sizeof *p);
    int r = world_find_region(&g->W, WR_START, 0);
    if (r >= 0) { memcpy(p->pos, g->W.reg[r].pos, 12); p->yaw = g->W.reg[r].yaw; }
    p->floor_tri = -1;
    p->grounded = 1;
    g->lock = -1;
    g->ccfg = cam_default_cfg();
    /* pinned between the demon and a column, a 0.8 m floor put the eye inside the column (whole frame one dark
       texture): stop at the obstacle instead, down to 0.35 m (> 0.3 m near plane) [придумал, 14.10] */
    g->ccfg.col_min_dist = 0.35f;
    g->row = cam_row(0);
    input_init(&g->inp, 0);
    body_play(&g->pb, kclip(g, P_IDLE, DIR_F), 0, 1);
    p->st = P_IDLE;
    /* save: continue a normal game; the autopilot always starts fresh */
    if (scenario == ASY_SC_NONE && !g->save_fn) {   /* the full game restores its save itself (ds_main.c) */
        static u8 buf[16384];
        SavePart parts[SAVE_MAX_PARTS];
        int np = save_read_file(SAVE_PATH, buf, sizeof buf, parts, SAVE_MAX_PARTS);
        if (np > 0) {
            const SavePart *pp = save_find(parts, np, SAVE_TAG_PROGRESS), *pe = save_find(parts, np, SAVE_TAG_EVENTS);
            int r1 = pp ? prg_save_read(&g->prg, pp->data, pp->size) : -99;
            int r2 = pe ? ev_save_read(&g->ev, pe->data, pe->size) : -99;
            asy_log(g, "save loaded: progress %d events %d souls %d", r1, r2, (int)g->prg.souls);
        } else {
            asy_log(g, "no save (%d): new game", np);
        }
    }
    if (g->has_boss) { g->B.st = BS_INACTIVE; g->boss_hidden = 1; g->boss_enabled = 1; }   /* MSB: enabled */
    if (g->has_ev) ev_start(&g->ev);
    float pos[3], yaw;
    if (scenario == ASY_SC_NONE && !g->save_fn && g->prg.deaths + g->prg.souls > 0 && ev_respawn(&g->ev, pos, &yaw)) {
        memcpy(p->pos, pos, 12);
        p->yaw = yaw;
    }
    if (g->has_col) {
        float h;
        int t = pcol_floor(&g->C, p->pos[0], p->pos[2], p->pos[1] + 1.5f, p->pos[1] - 4.0f, CAP.slope_cos, &h);
        if (t >= 0) { p->pos[1] = h; p->floor_tri = t; }
    }
    memcpy(p->safe_pos, p->pos, 12);
    if (g->has_col && p->floor_tri >= 0) g->row = cam_row(pcol_cam_row(&g->C, p->floor_tri));
    cam_reset(&g->cam, &g->ccfg, g->row, p->pos, p->yaw);
    g->autopilot = scenario != ASY_SC_NONE;
    if (g->autopilot) auto_init(g, scenario);
    asy_log(g, "start: player at (%.2f %.2f %.2f) yaw %.0f, scenario %s", p->pos[0], p->pos[1], p->pos[2],
            p->yaw * 180 / ASY_PI, auto_scenario_name(scenario));
}

/* ------------------------------------------------------------------ one step */

/* free fraction of a sphere r swept a -> b against a vertical cylinder (centre c, radius R, y in y0..y1); a start
   already inside is ignored (nothing to pull in to) */
static float cyl_cast(const float a[3], const float b[3], float r, const float c[3], float R, float y0, float y1)
{
    float ox = a[0] - c[0], oz = a[2] - c[2], dx = b[0] - a[0], dz = b[2] - a[2], dy = b[1] - a[1], rr = R + r;
    if (ox * ox + oz * oz <= rr * rr && a[1] >= y0 - r && a[1] <= y1 + r) return 1.0f;
    float A = dx * dx + dz * dz, B = 2 * (ox * dx + oz * dz), C = ox * ox + oz * oz - rr * rr;
    float t = 1.0f;
    if (A > 1e-8f) {
        float D = B * B - 4 * A * C;
        if (D < 0) return 1.0f;
        float t0 = (-B - sqrtf(D)) / (2 * A), t1 = (-B + sqrtf(D)) / (2 * A);
        if (t1 < 0 || t0 > 1) return 1.0f;
        if (t0 < 0) t0 = 0;   /* already inside the circle: only the vertical extent can be entered */
        if (t1 > 1) t1 = 1;
        /* y range within [t0, t1] */
        float ya = a[1] + dy * t0, yb = a[1] + dy * t1, lo = y0 - r, hi = y1 + r;
        if ((ya < lo && yb < lo) || (ya > hi && yb > hi)) return 1.0f;
        if (ya >= lo && ya <= hi) t = t0;
        else t = fabsf(dy) > 1e-6f ? ((ya < lo ? lo : hi) - a[1]) / dy : 1.0f;
    } else {   /* vertical segment inside the circle */
        if (C > 0) return 1.0f;
        float lo = y0 - r, hi = y1 + r;
        if (fabsf(dy) < 1e-6f) return 1.0f;
        t = ((a[1] < lo ? lo : hi) - a[1]) / dy;
        if (t < 0 || t > 1) return 1.0f;
    }
    return t < 0 ? 0 : t;
}

/* camera sphere cast: map collision + solid object boxes (doors, gates: not in the map .pcol, the camera used to go
   through the arena door and show its back as a black screen) + large decor props (the hall's columns have no map
   collision) + the demon's body [bug "camera in the Demon or a
   column", 14.10] */
static float cam_cast(void *ctx, const float a[3], const float b[3], float r)
{
    Game *g = ctx;
    g->cam_casts++;
    float f = pcol_sphere_cast(&g->C, a, b, r);
    g->cam_dbg[0] = f;
    float fo = world_obj_cast(&g->W, a, b, r);
    g->cam_dbg[1] = fo;
    if (fo < f) f = fo;
    fo = world_decor_cast(&g->W, a, b, r);
    g->cam_dbg[2] = fo;
    g->cam_dbg[3] = 1.0f;
    if (fo < f) f = fo;
    if (g->has_boss && g->boss_enabled && !g->boss_hidden && g->B.st != BS_INACTIVE && g->B.st != BS_DEAD) {
        /* body hull [придумал]: 0.8 x NpcParam HitRadius (2.5 -> 2.0 m), from the knees to 0.8 x height */
        float R = g->B.hdr->radius * 0.8f, y0 = g->B.pos[1] + 0.5f, y1 = g->B.pos[1] + g->B.hdr->height * 0.8f;
        float fb = cyl_cast(a, b, r, g->B.pos, R, y0, y1);
        g->cam_dbg[3] = fb;
        if (fb < f) f = fb;
    }
    return f;
}

static void step_hazards(Game *g, float dt)
{
    Player *p = &g->pl;
    /* iron ball: rolls p0 -> p1, sphere damage [урезание: straight line, no physics] */
    if (g->ball.on) {
        Ball *b = &g->ball;
        b->t += dt;
        float k = b->len > 0 ? b->t / b->len : 1;
        if (k > 1) k = 1;
        float c[3] = {b->p0[0] + (b->p1[0] - b->p0[0]) * k, b->p0[1] + (b->p1[1] - b->p0[1]) * k,
                      b->p0[2] + (b->p1[2] - b->p0[2]) * k};
        if (b->obj >= 0) { g->W.obj_m[b->obj][9] = c[0]; g->W.obj_m[b->obj][10] = c[1]; g->W.obj_m[b->obj][11] = c[2]; }
        float dd = asy_flat_dist(c, p->pos);
        if (!b->hit && dd < b->radius + CAP.radius && fabsf(c[1] - p->pos[1]) < 2.5f) {
            b->hit = 1;
            player_hit_external(g, 250.0f, 0, 60, 3, c, 1, "iron ball");   /* [придумал] */
        }
        if (k >= 1) b->on = 0;
    }
    /* firebombs: 1 s arc, 1.5 m blast [придумал: the trap itself is ours, see docs/events.md] */
    for (int i = 0; i < ASY_MAX_BOMB; i++) {
        Bomb *b = &g->bomb[i];
        if (!b->on) continue;
        b->t += dt;
        if (b->t >= b->len) {
            b->on = 0;
            if (asy_flat_dist(b->p1, p->pos) < 1.5f + CAP.radius && fabsf(b->p1[1] - p->pos[1]) < 2.0f)
                player_hit_external(g, 0, 60.0f, 30, 1, b->p1, 0, "firebomb");   /* [придумал] */
        }
    }
}

static void step_combat(Game *g, float dt)
{
    Player *p = &g->pl;
    CbActor *a = &g->pact;
    a->pos[0] = p->pos[0]; a->pos[1] = p->pos[1]; a->pos[2] = p->pos[2];
    a->yaw = p->yaw;
    a->bones = (const float *)g->pb.world;
    a->n_bones = g->kchr.hdr->n_bone;
    u32 id = clip_anim_id(g->pb.cur);
    if (p->st == P_FOG || p->st == P_LADDER || p->st == P_PRG || p->st == P_FALL) id = 0;
    if (id != g->p_anim_id) { a->anim_t_prev = 0; g->p_anim_id = id; }
    a->anim_id = id;
    a->anim_t = body_time(&g->pb, g->pb.cur, g->pb.t);
    a->guard_input = p->st == P_GUARD;
    for (int i = 0; i < g->n_en; i++) {
        Enemy *e = &g->en[i];
        CbActor *ea = &e->act;
        memcpy(ea->pos, e->pos, 12);
        ea->yaw = e->yaw;
        ea->bones = (const float *)e->body.world;
        ea->n_bones = g->hchr.hdr->n_bone;
        ea->anim_id = clip_anim_id(e->body.cur);
        ea->anim_t = body_time(&e->body, e->body.cur, e->body.t);
        e->attacked = 0;
    }
    if (g->has_boss && g->bidx >= 0) {
        CbActor *b = &g->bact;
        memcpy(b->pos, g->B.pos, 12);
        b->yaw = g->B.yaw;
        b->bones = (const float *)g->B.world;
        b->n_bones = g->B.anim.n_bones;
        b->anim_id = 0;          /* the demon's swings come from boss_hits(); here it is only a body to hit */
        b->anim_t = 0;
        b->hp = (float)g->B.hp;
        b->dead = !boss_alive(&g->B);
    }
    combat_update(&g->cbw, dt);
    for (int k = 0; k < g->cbw.n_ev; k++) {
        const CbEvent *ev = &g->cbw.ev[k];
        int pl_atk = ev->attacker == g->pidx, pl_vic = ev->victim == g->pidx;
        Enemy *ve = NULL;
        for (int i = 0; i < g->n_en; i++) if (g->en[i].cbi == ev->victim && ev->victim != g->pidx) ve = &g->en[i];
        int boss_vic = g->has_boss && ev->victim == g->bidx && g->bidx >= 0;
        if (ev->type == CB_EV_HIT || ev->type == CB_EV_THROW_HIT) fx_push(g, 1, ev->pos, g->pl.yaw);
        if (ev->type == CB_EV_BLOCK || ev->type == CB_EV_PARRY) fx_push(g, 2, ev->pos, g->pl.yaw);
        switch (ev->type) {
        case CB_EV_HIT:
        case CB_EV_THROW_HIT:
            if (pl_atk) g->hits_given++;
            if (pl_vic) g->hits_taken++;
            if (ve) {
                ve->attacked = 1;
                if (!ve->alerted) { ve->alerted = 1; if (ve->ai_ok) ve->ai.battle_state = 1; }
                if (ve->ai_ok) ai_agent_interrupt(g->ai, &ve->ai, 2);
            }
            if (boss_vic && pl_atk && ev->atk_type == CB_ATTACK_PLUNGE) {
                /* plunge window of the module (a23_3800, one hit): data damage x4 [версия: DS1 plunges hit far
                   harder than the AtkParam row gives - 71 vs ~290 of the former 4 x AR stub, combat.md 6.4] */
                float dmg = ev->damage * 4.0f;
                int hp0 = g->B.hp, inv = boss_iframes(&g->B), bst = g->B.st, bez = g->B.ez;
                float bt = g->B.t;
                boss_take_hit(&g->B, (int)(dmg + 0.5f), 100, BOSS_HIT_PLUNGE);
                g->bact.hp = (float)g->B.hp;
                g->bact.req_anim = 0;
                g->plunge_done = 1;
                if (g->B.hp < hp0) {
                    if (g->boss_lua) boss_ai_interrupt(&g->BA, BOSS_AI_INT_DAMAGED);
                    g->plunges++;
                    asy_log(g, "PLUNGE HIT on the Asylum Demon (combat module): %.1f x4 = %.0f dmg, demon hp %d/%d",
                            ev->damage, dmg, g->B.hp, g->B.hp_max);
                } else {
                    asy_log(g, "PLUNGE NO DAMAGE on the Asylum Demon (combat module): demon st %d ez %d t %.2f "
                            "iframes %d, hp %d/%d", bst, bez, bt, inv, g->B.hp, g->B.hp_max);
                }
            } else if (boss_vic) {
                boss_take_hit(&g->B, (int)(ev->damage + 0.5f), 20, BOSS_HIT_NORMAL);
                if (g->boss_lua) boss_ai_interrupt(&g->BA, BOSS_AI_INT_DAMAGED);
                g->bact.hp = (float)g->B.hp;
                g->bact.req_anim = 0;
            }
            asy_log(g, "%s: actor %d -> %d, %.1f dmg%s", ev->type == CB_EV_HIT ? "hit" : "THROW HIT", ev->attacker,
                    ev->victim, ev->damage, pl_vic ? " (player)" : boss_vic ? " (demon)" : "");
            if (pl_vic) prg_interrupt(&g->prg);
            break;
        case CB_EV_BLOCK:
            if (pl_vic) g->blocks++;
            asy_log(g, "BLOCK: actor %d blocks %d, %.1f dmg, stamina -%.1f", ev->victim, ev->attacker, ev->damage, ev->stamina);
            break;
        case CB_EV_GUARD_BREAK: asy_log(g, "guard break: actor %d", ev->victim); break;
        case CB_EV_PARRY:
            if (pl_atk) g->parries++;
            asy_log(g, "PARRY: actor %d parried %d", ev->attacker, ev->victim);
            if (ve && ve->ai_ok) ve->ez = -1;
            break;
        case CB_EV_STAGGER: if (g->verbose) asy_log(g, "stagger: actor %d", ev->victim); break;
        case CB_EV_REPEL: {   /* the attacker's weapon bounced off a shield (6.4): sparks, the hollow's action ends */
            fx_push(g, 2, ev->pos, g->pl.yaw);
            Enemy *ae = NULL;
            for (int i = 0; i < g->n_en; i++) if (g->en[i].cbi == ev->attacker && ev->attacker != g->pidx) ae = &g->en[i];
            if (ae && ae->ai_ok) ae->ez = -1;
            if (ev->victim == g->pidx) g->blocks++;
            asy_log(g, "REPEL: actor %d bounced off actor %d's shield", ev->attacker, ev->victim);
            break;
        }
        case CB_EV_INVULN:
            if (pl_vic) g->rolls_iframe++;
            asy_log(g, "i-frames: actor %d dodged a hit from %d", ev->victim, ev->attacker);
            break;
        case CB_EV_THROW: asy_log(g, "throw: actor %d on %d", ev->attacker, ev->victim); break;
        case CB_EV_DEATH:
            if (pl_vic) player_die(g, "combat");
            if (ve) {
                g->kills++;
                int souls = ve->act.npc ? ve->act.npc->souls : 20;
                prg_add_souls(&g->prg, souls);
                asy_log(g, "hollow %u killed: +%d souls -> %d", (unsigned)ve->entity, souls, (int)g->prg.souls);
            }
            if (boss_vic) asy_log(g, "combat: the demon's body at 0 hp");
            break;
        }
    }
    if (g->has_boss && g->bidx >= 0) { g->bact.req_anim = 0; g->bact.push_t = 0; }
}

static void sync_world_objects(Game *g)
{
    for (int i = 0; i < (int)g->ev.n_ent; i++) {
        int wi = g->ev_obj_world[i];
        if (wi < 0) continue;
        EvObjInfo o;
        if (!ev_obj_get(&g->ev, i, &o)) continue;
        int kind = g->W.obj[wi].kind;
        int on = o.enabled && !o.destroyed;
        if (kind == WK_DOOR || kind == WK_CELL_DOOR || kind == WK_GATE) on = on && !o.open;   /* [урезание] open = gone */
        if (kind == WK_BOULDER && g->ball.on) on = 1;
        if (g->W.obj_on[wi] != on) world_obj_enable(&g->W, wi, on);
        /* DS1 11810211 (EMEVD [проверено]): after the roll the ball 1811215 is disabled and 1811216 enabled with
         * EndOfAnimation, i.e. the rolled ball lies where its animation ends. There are no object animations here,
         * so 1811216 stood at its MSB spot on the stairs top and walled off the way to the east wing door (the
         * route bot stood there under the firebombs, fought on the stairs and fell). Put it past the broken wall
         * 1811210, along the roll [придумал: 2.5 m beyond the wall, out of the way to Oscar]. */
        if (kind == WK_BOULDER && on && o.id == 1811216 && !(g->ball.on && g->ball.obj == wi)) {
            int wall = world_find_obj(&g->W, 1811210);
            float *m = g->W.obj_m[wi];
            if (wall >= 0 && m[10] > g->W.obj[wall].pos[1] + 2.0f) {
                const float *a = o.pos, *b = g->W.obj[wall].pos;
                float dx = b[0] - a[0], dz = b[2] - a[2], l = sqrtf(dx * dx + dz * dz) + 1e-4f;
                m[9] = b[0] + dx / l * 2.5f;
                m[10] = b[1] + 0.58f;   /* centre over the floor, as at its MSB spot (209.01 over 208.43) */
                m[11] = b[2] + dz / l * 2.5f;
                asy_log(g, "rolled iron ball 1811216 placed past the broken wall (%.2f %.2f %.2f)", m[9], m[10], m[11]);
            }
        }
    }
}

void game_step(Game *g, const RawPad *raw_in)
{
    float dt = ASY_DT;
    Player *p = &g->pl;
    RawPad raw = *raw_in;
    g->n_fx = 0;
    if (g->autopilot) auto_pad(g, &raw);
    g->raw_pressed = raw.buttons & ~g->raw_buttons;
    g->raw_buttons = raw.buttons;
    input_update(&g->inp, &raw, dt, &g->in);
    AshInput *in = &g->in;

    /* lock-on: L tap */
    if (in->pressed & ACT_LOCK) {
        if (g->lock >= 0) {
            if (fabsf(in->move_x) < 0.6f) { g->lock = -1; if (g->verbose) asy_log(g, "unlock"); }
        } else {
            int t = lock_acquire(g);
            if (t >= 0) { g->lock = t; asy_log(g, "lock-on %s", t == 100 ? "Asylum Demon" : "hollow"); }
        }
    }
    if (g->lock >= 0) {
        float tp[3];
        if (!lock_target_pos(g, g->lock, tp) || asy_flat_dist(p->pos, tp) > 24.0f) g->lock = -1;
    }

    player_tick(g, dt);
    for (int i = 0; i < g->n_en; i++) en_tick(g, &g->en[i], i, dt);
    boss_step(g, dt);
    step_hazards(g, dt);
    game_pose(g);              /* bones of this step for the hit volumes (and for drawing) */
    step_combat(g, dt);

    /* events */
    EvInput ei;
    memcpy(ei.pos, p->pos, 12);
    ei.yaw = p->yaw;
    ei.interact = !prg_locked(&g->prg) && !p->dead && (in->pressed & ACT_INTERACT) != 0;
    ei.floor_part = (g->has_col && p->floor_tri >= 0) ? g->C.part[g->C.tri[p->floor_tri].part].name : NULL;
    if (g->ev.pending_reload) {   /* the reload tick restarts the events: they must see the respawn spot, not the
                                     place of death (else the first-encounter trigger fires at once) */
        float rp[3], ry;
        if (ev_respawn(&g->ev, rp, &ry)) { memcpy(ei.pos, rp, 12); ei.yaw = ry; }
        ei.floor_part = NULL;
    }
    if (g->has_ev) {
        ev_tick(&g->ev, dt, &ei);
        sync_world_objects(g);
    } else if (g->zone_step) {
        g->zone_step(g, dt);
    }

    /* progression */
    PrgInput pi;
    memset(&pi, 0, sizeof pi);
    memcpy(pi.pos, p->pos, 12);
    pi.item = (in->pressed & ACT_ITEM) && asy_has_estus(g) && !p->dead;
    pi.up = (g->raw_pressed & PSP_CTRL_UP) != 0;
    pi.down = (g->raw_pressed & PSP_CTRL_DOWN) != 0;
    pi.confirm = (g->raw_pressed & PSP_CTRL_CROSS) != 0;
    pi.cancel = (g->raw_pressed & PSP_CTRL_CIRCLE) != 0;
    if (g->ext_menu) {   /* src/menu owns the bonfire menu: only its "leave" reaches progress */
        pi.up = pi.down = pi.confirm = 0;
        pi.cancel = g->ext_leave_bonfire && g->prg.phase == PRG_REST && g->prg.sub == 1;
        if (pi.cancel || g->prg.phase != PRG_REST) g->ext_leave_bonfire = 0;
    }
    pi.busy = p->st >= P_ROLL && p->st != P_GUARD && p->st != P_PRG;
    int estus_before = g->prg.estus;
    prg_tick(&g->prg, &pi, dt);
    if (g->prg.estus < estus_before) { g->estus_used++; asy_log(g, "Estus: drink (%d left), hp %.0f", (int)g->prg.estus, g->pact.hp); }
    if ((g->prg.req & PRG_REQ_ANIM) && g->has_prg) {
        int c = anim_find(&g->kprg, prg_anim_name[g->prg.anim]);
        if (c >= 0) {
            body_play(&g->pb, (ClipRef){&g->kprg, c}, 0.1f, 1);
            g->pb.hold = !g->prg.anim_loop;
            p->st = P_PRG;
        }
    }
    if (g->prg.req & PRG_REQ_RELOAD) {
        asy_log(g, "death screen done -> reload");
        if (g->has_ev) ev_player_died(&g->ev); else game_reload(g);
    }
    if (g->prg.req & PRG_REQ_SAVE) game_save(g, "progress");
    if (g->prg.req & PRG_REQ_SOULS_BACK) asy_log(g, "BLOODSTAIN picked up: souls %d", (int)g->prg.souls);
    if (g->prg.req & PRG_REQ_STAND_DONE) { g->pb.hold = 0; player_set(g, P_IDLE, DIR_F); }

    /* visibility cell + camera */
    if (g->has_col && p->floor_tri >= 0) {
        if (g->zone == ASY_ZONE_ASYLUM) world_set_cell_by_part(&g->W, g->C.tri[p->floor_tri].part);   /* Firelink: all */
        g->row = cam_row(pcol_cam_row(&g->C, p->floor_tri));
    }
    CamInput ci;
    memset(&ci, 0, sizeof ci);
    memcpy(ci.char_pos, p->pos, 12);
    ci.char_facing = p->yaw;
    ci.move_heading = p->move_heading;
    ci.speed_frac = p->speed_frac;
    ci.locomotion = p->st == P_WALK || p->st == P_JOG || p->st == P_RUN;
    ci.in_action = p->st >= P_ROLL;
    ci.reset = g->lock < 0 && (in->pressed & (ACT_LOCK | ACT_CAM_RESET)) != 0;
    ci.row = g->row;
    ci.locked = g->lock >= 0;
    if (g->lock >= 0) {
        lock_point(g, g->lock, g->lock_pt);
        memcpy(ci.target, g->lock_pt, 12);
        ci.target_radius = g->lock == 100 ? 2.5f : 0.5f;
    }
    ci.cast = g->has_col ? cam_cast : NULL;
    ci.cast_ctx = g;
    ci.yaw_in = in->cam_yaw_in;
    ci.pitch_in = in->cam_pitch_in;
    cam_update(&g->cam, &g->ccfg, &ci, dt);

    /* overlays */
    if (g->toast_text) { g->toast_t += dt; if (g->toast_t > 3.0f) g->toast_text = NULL; }
    if (g->ev_fade_len > 0) {
        g->ev_fade_t += dt;
        float h = g->ev_fade_len * 0.5f, t = g->ev_fade_t;
        g->ev_fade = t < h ? t / h : 1.0f - (t - h) / h;
        if (t >= g->ev_fade_len) { g->ev_fade = 0; g->ev_fade_len = 0; }
    }
    if (g->end_demo) g->end_t += dt;
    g->frame++;
    g->time += dt;
}

void game_pose(Game *g)
{
    body_pose(&g->pb);
    for (int i = 0; i < g->n_en; i++)
        if (g->en[i].enabled) body_pose(&g->en[i].body);
}

/* ------------------------------------------------------------------ full game: class, items, zone change (ds_main.c) */

void game_set_class(Game *g, int class_row, int gift_row, const u8 attr[8], int level)
{
    g->class_row = class_row;
    g->level = level;
    memcpy(g->attr, attr, 8);
    if (!g->has_items) return;
    /* [урезание] the full CharaInitParam kit instead of the DS1 Asylum start (hilt, no shield): the class model
       (cls_<row>.ashc) wears and holds exactly this kit, and the Asylum fights before bonfire 2 need the shield.
       The Estus flask is taken out - the story gives it at Oscar (EV_CMD_ITEM 4:200). */
    itm_new_game(&g->inv, &g->idb, class_row, gift_row, 0);
    itm_remove(&g->inv, KEY_ESTUS, 99);
    itm_remove(&g->inv, KEY_ESTUS + 1, 99);
    g->inv.estus = &g->prg.estus;
    game_apply_equipment(g);
    g->pact.hp = g->pact.hp_max;
    g->pact.sp = g->pact.sp_max;
    asy_log(g, "class %d (gift %d) level %d: %d inventory entries, R %08x L %08x, hp %.0f stamina %.0f AR %.1f",
            class_row, gift_row, level, g->inv.n, (unsigned)itm_right_weapon(&g->inv), (unsigned)itm_left_weapon(&g->inv),
            g->pact.hp_max, g->pact.sp_max, g->ist.ar_r[0]);
}

void game_apply_equipment(Game *g)
{
    if (!g->has_items || !g->class_row) return;
    CbActor *a = &g->pact;
    itm_calc(&g->inv, &g->idb, g->attr, &g->ist);
    const ItmStats *s = &g->ist;
    float hpf = a->hp_max > 0 ? a->hp / a->hp_max : 1.0f;
    if (s->hp_max > 0) { a->hp_max = s->hp_max; a->hp = hpf * a->hp_max; }
    if (s->sp_max > 0) { a->sp_max = s->sp_max; if (a->sp > a->sp_max) a->sp = a->sp_max; }
    /* [урезание] combat data has the Longsword / Knight Shield records only (moveset, hit windows): every class
       swings with them, the numbers (attack rating, guard, stability, defence) come from its own equipment */
    const CbWeapon *wr = cb_weapon_find(&g->cbd, 201000), *wl = cb_weapon_find(&g->cbd, 1451000);
    if (wr) {
        g->wep_r_ovr = *wr;
        for (int k = 0; k < 4; k++) g->wep_r_ovr.ar[k] = s->ar_r[k];
        if (g->wep_r_ovr.ar[0] < 1) g->wep_r_ovr.ar[0] = 1;
        a->wep_r = &g->wep_r_ovr;
    }
    if (wl) {
        g->wep_l_ovr = *wl;
        if (itm_left_weapon(&g->inv)) {
            for (int k = 0; k < 4; k++) g->wep_l_ovr.guard_cut[k] = s->guard_l[k];
            g->wep_l_ovr.stability = (uint16_t)(s->stability_l > 0 ? s->stability_l : 1);
        } else {   /* no shield: a weapon block [версия] */
            for (int k = 0; k < 4; k++) g->wep_l_ovr.guard_cut[k] = k ? 10.0f : 40.0f;
            g->wep_l_ovr.stability = 20;
        }
        a->wep_l = &g->wep_l_ovr;
    }
    if (s->def[0] > 0) for (int k = 0; k < 4; k++) a->def[k] = s->def[k];
}

void game_use_item(Game *g, int idx)
{
    if (!g->has_items) return;
    ItmUse u;
    int r = itm_use(&g->inv, &g->idb, idx, &u);
    if (r != ITM_OK) { asy_log(g, "use item %d refused (%d)", idx, r); return; }
    switch (u.kind) {
    case ITM_USE_ESTUS: prg_try_estus(&g->prg); break;
    case ITM_USE_SOULS: prg_add_souls(&g->prg, u.value); break;
    case ITM_USE_HUMANITY: g->pact.hp = g->pact.hp_max; break;   /* DS1: humanity heals fully */
    case ITM_USE_HEAL: g->pact.hp += (float)u.value; if (g->pact.hp > g->pact.hp_max) g->pact.hp = g->pact.hp_max; break;
    default: break;
    }
    if (u.kind != ITM_USE_ESTUS && itm_used_text(&g->idb, u.key, g->toast_buf, 96) > 0) {
        g->toast_text = g->toast_buf;
        g->toast_t = 0;
    }
    asy_log(g, "used item %08x kind %d value %d -> souls %d hp %.0f", (unsigned)u.key, u.kind, u.value, (int)g->prg.souls,
            g->pact.hp);
}

void game_place_player(Game *g, const float pos[3], float yaw)
{
    Player *p = &g->pl;
    memcpy(p->pos, pos, 12);
    p->yaw = yaw;
    p->vy = 0;
    p->floor_tri = -1;
    if (g->has_col) {
        float h;
        int t = pcol_floor(&g->C, p->pos[0], p->pos[2], p->pos[1] + 1.5f, p->pos[1] - 4.0f, CAP.slope_cos, &h);
        if (t >= 0) { p->pos[1] = h; p->floor_tri = t; }
    }
    p->grounded = 1;
    memcpy(p->safe_pos, p->pos, 12);
    if (g->has_col && p->floor_tri >= 0) g->row = cam_row(pcol_cam_row(&g->C, p->floor_tri));
    cam_reset(&g->cam, &g->ccfg, g->row, p->pos, p->yaw);
    if (!prg_locked(&g->prg)) player_set(g, P_IDLE, DIR_F);
}

void game_reload_now(Game *g) { game_reload(g); }

int game_set_body(Game *g, AsyLoadFn load, const char *ashc)
{
    u8 *f;
    Chr c;
    int n = load_into(load, ashc, &f, 0);
    memset(&c, 0, sizeof c);
    if (n <= 0 || chr_load(&c, f, n) || c.hdr->n_bone != g->kchr.hdr->n_bone) {
        free(c.inv_bind);
        if (n > 0) free(f);
        asy_log(g, "body %s not usable: the knight model stays", ashc);
        return -1;
    }
    free(g->f_body);
    free(g->kchr.inv_bind);
    g->f_body = f;
    g->kchr = c;
    cb_actor_bind(&g->pact, &g->cbd, bone_lookup, &g->kchr);
    g->pb.chr = &g->kchr;
    asy_log(g, "player body %s: %u bones", ashc, (unsigned)g->kchr.hdr->n_bone);
    return 0;
}

void game_zone_unload(Game *g)
{
    /* the zone world + collision */
    free(g->W.tpix); free(g->W.tclut); free(g->W.obj_on); free(g->W.obj_m); free(g->W.part_col);
    free(g->f_pak);
    g->f_pak = NULL;
    memset(&g->W, 0, sizeof g->W);
    if (g->has_col) { free(g->C.plane); free(g->C.stamp); }
    memset(&g->C, 0, sizeof g->C);
    g->has_col = 0;
    /* events */
    if (g->has_ev) { free(g->ev_mem); free(g->ev_obj_world); free(g->f_ev); }
    g->ev_mem = NULL; g->ev_obj_world = NULL; g->f_ev = NULL;
    memset(&g->ev, 0, sizeof g->ev);
    g->has_ev = 0;
    /* enemies: hollows, the demon, their Lua; the combat world keeps the player only */
    if (g->has_boss && g->boss_lua && g->ai) ai_agent_free(g->ai, &g->BA.agent);
    for (int i = 0; i < g->n_en; i++) if (g->en[i].ai_ok && g->ai) ai_agent_free(g->ai, &g->en[i].ai);
    if (g->ai) ai_host_free(g->ai);
    g->ai = NULL;
    if (g->has_hollow) free(g->hchr.inv_bind);
    if (g->has_boss) free(g->B.chr.inv_bind);
    for (int i = 0; i < 3; i++) { free(g->f_hollow[i]); g->f_hollow[i] = NULL; }
    for (int i = 0; i < 4; i++) { free(g->f_boss[i]); g->f_boss[i] = NULL; }
    g->has_hollow = g->has_hcmb = 0;
    g->n_en = 0;
    g->has_boss = g->boss_lua = g->boss_enabled = g->boss_ai_on = 0;
    g->boss_hidden = 1;
    g->boss_bar = 0;
    g->bidx = -1;
    g->cbw.n_actor = g->pidx + 1;
    g->lock = -1;
    g->ball.on = 0;
    for (int i = 0; i < ASY_MAX_BOMB; i++) g->bomb[i].on = 0;
    g->end_demo = 0;
    g->toast_text = NULL;
    asy_log(g, "zone %d unloaded", g->zone);
}

int game_zone_load_plain(Game *g, AsyLoadFn load, const char *pak, int zone, const float pos[4])
{
    u8 *f;
    int n = load_into(load, pak, &f, 1);
    if (n <= 0 || world_load(&g->W, f, n)) { printf("ash-asy: zone %s bad\n", pak); return -1; }
    g->f_pak = f;
    g->has_col = g->W.pcol && pcol_load(&g->C, g->W.pcol, g->W.pcol_size) == 0;
    g->zone = zone;
    g->has_ev = 0;
    g->void_y = pos[1] - 40.0f;   /* [придумал] 40 m below the entry point = fell off the zone */
    memcpy(g->respawn, pos, 16);
    world_set_cell(&g->W, -1);
    game_place_player(g, pos, pos[3]);
    printf("ash-asy: zone %s %d KB: %u batches, %u objs, %u chrs, %u regions; pcol %s %u tris\n", pak, n / 1024,
           (unsigned)g->W.w->n_batch, (unsigned)g->W.z->n_obj, (unsigned)g->W.z->n_chr, (unsigned)g->W.z->n_reg,
           g->has_col ? "ok" : "MISSING", g->has_col ? (unsigned)g->C.hdr->n_tri : 0u);
    return 0;
}
