/*
 * Asylum Demon runtime (boss.h). The state machine mirrors FromSoftware's 223200_battle.lua (DSR m18_01 luabnd,
 * decompiled by tools/ai/extract_ai.py; docs/boss.md section 3 has the table with line references) plus the
 * aiCommon helpers it calls (Approach_Act, Approach_and_Attack_Act, Common_Battle_Activate, FindAttack_Step,
 * Damaged_Step, UseItem_Act). Native goal behaviour (CommonAttack 2200 turning, ApproachTarget, SidewayMove) is
 * not in the data: our reconstruction, tagged [версия].
 */
#include "boss.h"

#include <math.h>
#include <pspgum.h>
#include <stdio.h>
#include <string.h>

#define PI 3.14159265f
#define FPS 30.0f
static const float BLEND = 6.0f / 30.0f;
static const float WALK_SWITCH_RUN_MARGIN = 6.0f; /* DashDist = AppDist + 6 in every act of 223200 */

/* ------------------------------------------------------------------ small helpers */

static float wrap_pi(float a)
{
    while (a > PI) a -= 2 * PI;
    while (a < -PI) a += 2 * PI;
    return a;
}

static u32 rnd(Boss *b)
{
    b->rng = b->rng * 1664525u + 1013904223u;
    return b->rng >> 8;
}
static int rand_int(Boss *b, int lo, int hi) { return lo + (int)(rnd(b) % (u32)(hi - lo + 1)); } /* GetRandam_Int */
static float rand_float(Boss *b, float lo, float hi) { return lo + (hi - lo) * (float)(rnd(b) & 0xffff) / 65535.0f; }

static void to_world(const Boss *b, const float m[3], float out[3])
{
    float s = sinf(b->yaw), c = cosf(b->yaw);
    out[0] = b->pos[0] + c * m[0] + s * m[2];
    out[1] = b->pos[1] + m[1];
    out[2] = b->pos[2] - s * m[0] + c * m[2];
}

static const BossFileReg *find_reg(const Boss *b, int id)
{
    for (u32 i = 0; i < b->hdr->n_reg; i++)
        if (b->reg[i].id == id) return &b->reg[i];
    return NULL;
}

int boss_in_region(const Boss *b, int id, const float p[3])
{
    const BossFileReg *r = find_reg(b, id);
    if (!r) return 0;
    float dx = p[0] - r->pos[0], dy = p[1] - r->pos[1], dz = p[2] - r->pos[2];
    if (r->shape == 1) return dx * dx + dy * dy + dz * dz <= r->size[0] * r->size[0];
    /* box: pos = bottom centre [версия], a little slack under the floor for feet slightly below it */
    return fabsf(dx) <= r->size[0] * 0.5f && fabsf(dz) <= r->size[1] * 0.5f && dy >= -0.5f && dy <= r->size[2];
}

int boss_region_center(const Boss *b, int id, float out[3])
{
    const BossFileReg *r = find_reg(b, id);
    if (!r) return -1;
    out[0] = r->pos[0]; out[1] = r->pos[1]; out[2] = r->pos[2];
    return 0;
}

int boss_dummy_world(const Boss *b, int i, float out[3])
{
    if (i < 0 || i >= (int)b->hdr->n_dmy) return -1;
    const BossFileDmy *d = &b->dmy[i];
    const ScePspFMatrix4 *m = &b->skin[d->bone]; /* skin = world * inv_bind: bind-pose model point -> posed */
    float p[3] = {d->pos[0], d->pos[1], d->pos[2]}, q[3];
    q[0] = m->x.x * p[0] + m->y.x * p[1] + m->z.x * p[2] + m->w.x;
    q[1] = m->x.y * p[0] + m->y.y * p[1] + m->z.y * p[2] + m->w.y;
    q[2] = m->x.z * p[0] + m->y.z * p[1] + m->z.z * p[2] + m->w.z;
    to_world(b, q, out);
    return 0;
}

static int dummy_index(const Boss *b, int id)
{
    for (u32 i = 0; i < b->hdr->n_dmy; i++)
        if (b->dmy[i].id == id) return (int)i;
    return -1;
}

/* ------------------------------------------------------------------ animation */

static void play(Boss *b, int clip, float blend)
{
    if (clip < 0) clip = b->clip_idle;
    if (clip == b->clip && blend > 0 && (b->anim.clips[clip].flags & CLIP_LOOP)) return;
    b->prev_clip = blend > 0 ? b->clip : -1;
    b->prev_t = b->t;
    b->clip = clip;
    b->t = 0;
    b->t_prev_tick = 0;
    b->blend = blend > 0 ? 0 : BLEND;
    b->swing_base += 16;
}

static float clip_len(const Boss *b, int clip) { return anim_duration(&b->anim, clip); }

/* window of `kind` covering frame f of the current clip, NULL if none */
static const BossFileWin *window_at(const Boss *b, int kind, float f)
{
    for (u32 i = 0; i < b->hdr->n_win; i++) {
        const BossFileWin *w = &b->win[i];
        if (w->clip == b->clip && w->kind == kind && f >= w->f0 && f < w->f1) return w;
    }
    return NULL;
}

static float cancel_time(const Boss *b, int clip)
{
    for (u32 i = 0; i < b->hdr->n_win; i++)
        if (b->win[i].clip == clip && b->win[i].kind == BW_CANCEL) return b->win[i].f0 / FPS;
    return clip_len(b, clip);
}

static void pose(Boss *b)
{
    float w = b->blend >= BLEND ? 1.0f : b->blend / BLEND;
    anim_sample(&b->anim, b->clip, b->t, b->pose_a);
    if (w < 1.0f && b->prev_clip >= 0) {
        anim_sample(&b->anim, b->prev_clip, b->prev_t, b->pose_b);
        anim_blend(b->anim.n_bones, b->pose_b, b->pose_a, w, b->pose_a);
    }
    anim_skin_matrices(b->anim.n_bones, b->chr.parent, b->pose_a, b->chr.inv_bind, b->world, b->skin);
}

/* ------------------------------------------------------------------ load / reset */

static int clip_named(const Boss *b, const char *n)
{
    int c = anim_find(&b->anim, n);
    if (c < 0) printf("ash-boss: clip %s missing\n", n);
    return c;
}

int boss_load(Boss *b, u8 *chr_file, int chr_size, u8 *anim_file, int anim_size, u8 *boss_file, int boss_size,
              u8 *name_file, int name_size)
{
    memset(b, 0, sizeof(*b));
    if (chr_load(&b->chr, chr_file, chr_size) < 0) return -1;
    if (anim_load(&b->anim, anim_file, anim_size) < 0) return -2;
    if (b->anim.n_bones != (int)b->chr.hdr->n_bone) return -3;
    if (boss_size < (int)sizeof(BossFileHdr) || memcmp(boss_file, "ASHB", 4) != 0) return -4;
    b->file = boss_file;
    b->hdr = (const BossFileHdr *)boss_file;
    if (b->hdr->version != 1) return -5;
    b->atk = (const BossFileAtk *)(boss_file + b->hdr->off_atk);
    b->win = (const BossFileWin *)(boss_file + b->hdr->off_win);
    b->dmy = (const BossFileDmy *)(boss_file + b->hdr->off_dmy);
    b->reg = (const BossFileReg *)(boss_file + b->hdr->off_reg);
    b->clip_idle = clip_named(b, "idle");
    b->clip_walk = clip_named(b, "walk");
    b->clip_walk_l = clip_named(b, "walk_l");
    b->clip_walk_r = clip_named(b, "walk_r");
    b->clip_run = clip_named(b, "run");
    b->clip_turn[0] = clip_named(b, "turn_l90");
    b->clip_turn[1] = clip_named(b, "turn_r90");
    b->clip_turn[2] = clip_named(b, "turn_l180");
    b->clip_turn[3] = clip_named(b, "turn_r180");
    b->clip_death = clip_named(b, "death");
    b->clip_entrance = clip_named(b, "ev9060");
    b->clip_step = clip_named(b, "step_b");
    for (int i = 0; i < 32; i++) {
        char n[16];
        snprintf(n, sizeof(n), "a%d", 3000 + i);
        b->clip_of_ez[i] = anim_find(&b->anim, n);
    }
    if (b->clip_idle < 0 || b->clip_walk < 0 || b->clip_run < 0 || b->clip_death < 0) return -6;
    b->dmy_rfoot = dummy_index(b, 3);
    b->dmy_lfoot = dummy_index(b, 4);
    b->lock_bone = chr_bone(&b->chr, "Spine1");
    if (b->lock_bone < 0) b->lock_bone = 0;
    if (name_file && name_size > 16) {
        const u16 *h = (const u16 *)name_file;
        if (16 + h[0] * h[1] * 2 <= name_size) {
            b->name_w = h[0]; b->name_h = h[1]; b->name_tw = h[2];
            b->name_px = (const u16 *)(name_file + 16); /* 16-aligned: the file buffer is 64-aligned */
        }
    }
    b->hp_max = (int)b->hdr->hp;
    b->rng = 0x2232u;
    printf("ash-boss: c2232 hp=%d turn=%.0f deg/s r=%.1f h=%.1f atk=%u win=%u dmy=%u reg=%u clips=%u name %dx%d\n",
           b->hp_max, b->hdr->turn_deg_s, b->hdr->radius, b->hdr->height, (unsigned)b->hdr->n_atk,
           (unsigned)b->hdr->n_win, (unsigned)b->hdr->n_dmy, (unsigned)b->hdr->n_reg,
           (unsigned)b->anim.hdr->n_clips, b->name_w, b->name_h);
    boss_reset(b, NULL, 0);
    return 0;
}

void boss_reset(Boss *b, const float *pos, float yaw)
{
    if (pos) {
        b->pos[0] = pos[0]; b->pos[1] = pos[1]; b->pos[2] = pos[2]; b->yaw = yaw;
    } else {
        /* MSB c2232_0000: (3.0, 198.06, -29.54) rot 180 -> PSP x mirrored, facing +z (into the hall) */
        b->pos[0] = -3.0f; b->pos[1] = 198.06f; b->pos[2] = -29.54f; b->yaw = PI;
    }
    b->hp = b->hp_max;
    b->hp_trail = (float)b->hp_max;
    b->st = BS_WAIT;
    b->engaged = 0;
    b->visible = 1;
    b->ez = -1;
    b->n_plan = b->plan_i = 0;
    b->well_space = -1;
    b->number[0] = b->number[1] = 0;
    b->clip = -1;
    play(b, b->clip_idle, 0);
    pose(b);
    snprintf(b->log, sizeof(b->log), "reset");
}

void boss_enter_drop(Boss *b)
{
    float p[3];
    if (b->clip_entrance < 0 || boss_region_center(b, BREG_SKY, p) < 0) return;
    b->pos[0] = p[0]; b->pos[1] = p[1]; b->pos[2] = p[2]; b->yaw = PI; /* MSB rot 180 */
    b->st = BS_ENTRANCE;
    b->visible = 1;
    b->engaged = 1;
    play(b, b->clip_entrance, 0);
    pose(b);
    snprintf(b->log, sizeof(b->log), "entrance: drop from the ceiling (ev9060)");
}

/* ------------------------------------------------------------------ perception */

static void perceive(Boss *b, const BossWorld *w)
{
    b->player[0] = w->player[0]; b->player[1] = w->player[1]; b->player[2] = w->player[2];
    float dx = w->player[0] - b->pos[0], dz = w->player[2] - b->pos[2];
    /* GetDist: distance between the bodies (the script uses negative ranges, e.g. Att3008 -3..0) [версия] */
    b->dist = sqrtf(dx * dx + dz * dz) - b->hdr->radius - w->player_radius;
    float want = atan2f(-dx, -dz);
    b->angle = wrap_pi(want - b->yaw);
}

static int observe(const Boss *b, int dmy, float radius) /* ai:IsInsideObserve of AddObserveChrDmySphere */
{
    float p[3];
    if (boss_dummy_world(b, dmy, p) < 0) return 0;
    float dx = b->player[0] - p[0], dy = b->player[1] - p[1], dz = b->player[2] - p[2];
    return dx * dx + dy * dy + dz * dz <= radius * radius;
}

/* ------------------------------------------------------------------ the script (223200_battle.lua) */

static void plan_add(Boss *b, int kind, int ez, float dist, float life, int walk_only)
{
    if (b->n_plan >= BOSS_MAX_PLAN) return;
    BossStep *s = &b->plan[b->n_plan++];
    memset(s, 0, sizeof(*s));
    s->kind = (u8)kind; s->ez = (s16)ez; s->dist = dist; s->life = life; s->walk_only = (u8)walk_only;
}

/* Approach_Act(ai, goal, AppDist, AppDist + 6, 0): ApproachTarget, walk unless the target is beyond DashDist */
static void approach(Boss *b, float app) { plan_add(b, SG_APPROACH, 0, app, 10, 0); }

/* distances of the script's locals (223200_battle.lua lines 5-26) */
#define D3000 9.6f
#define D3001 8.0f
#define D3002 6.6f
#define D3003 6.4f
#define D3006 1.5f
#define D3007 11.9f
#define D3014 8.0f

static int act(Boss *b, int n)
{
    int fate = rand_int(b, 1, 100);
    switch (n) {
    case 1: approach(b, D3007); plan_add(b, SG_ATTACK, 3007, 0, 10, 0); return 100;
    case 2:
        approach(b, D3002);
        plan_add(b, SG_ATTACK, 3002, 0, 10, 0);
        if (fate > 10) plan_add(b, SG_COMBO, 3011, 0, 10, 0);
        if (fate > 20) plan_add(b, SG_COMBO, 3012, 0, 10, 0);
        return 100;
    case 3: approach(b, D3006); plan_add(b, SG_ATTACK, 3006, 0, 10, 0); return 100;
    case 4: plan_add(b, SG_ATTACK, 3008, 0, 10, 0); return 100;                  /* AttackTunableSpin */
    case 5:
        approach(b, D3000);
        plan_add(b, SG_ATTACK, 3000, 0, 10, 0);
        if (fate > 10) plan_add(b, SG_COMBO, 3010, 0, 10, 0);
        return 100;
    case 6: approach(b, D3014); plan_add(b, SG_ATTACK, 3014, 0, 10, 0); return 0;
    case 7: approach(b, D3001); plan_add(b, SG_ATTACK, 3001, 0, 10, 0); return 100; /* Approach_and_Attack_Act */
    case 8: plan_add(b, SG_ATTACK, 3004, 0, 10, 0); return 100;
    case 9: plan_add(b, SG_ATTACK, 3005, 0, 10, 0); return 100;
    case 10: plan_add(b, SG_NONSPIN, 701, 0, 5, 0); return 0;                    /* SpinStep 701 back */
    case 11:
        approach(b, D3003);
        plan_add(b, SG_ATTACK, 3003, 0, 10, 0);
        if (fate > 10) plan_add(b, SG_COMBO, 3013, 0, 10, 0);
        return 100;
    case 12: plan_add(b, SG_NONSPIN, 3020, 0, 10, 0); return 0;
    case 13: plan_add(b, SG_NONSPIN, 3021, 0, 10, 0); return 0;
    case 14: plan_add(b, SG_NONSPIN, 3022, 0, 10, 0); return 0;
    case 15: plan_add(b, SG_NONSPIN, 3023, 0, 10, 0); return 0;
    case 16: /* SetEventMoveTarget(1812301); ApproachTarget(POINT_EVENT, 2, walk); Wait 0.5..2; Turn */
        plan_add(b, SG_GOTO, BREG_HOME, 2, 5, 1);
        plan_add(b, SG_WAIT, 0, 0, rand_float(b, 0.5f, 2.0f), 0);
        plan_add(b, SG_TURN, 0, 0, 2, 0);
        return 0;
    }
    return 0;
}

static void think(Boss *b)
{
    int per[17];
    memset(per, 0, sizeof(per));
    const float d = b->dist;
    const float *pp = b->player;
    int on_balcony = boss_in_region(b, BREG_BALCONY, pp);
    if (on_balcony && b->number[0] == 0) {
        per[12] = 100; b->number[0] = 1;
    } else if (on_balcony && b->number[0] == 1 && b->number[1] <= 2) {
        per[13] = 100; b->number[1]++;
    } else if (boss_in_region(b, BREG_BATTLE, pp) && b->number[0] == 1) {
        per[14] = 100; b->number[0] = 0; b->number[1] = 0;
    } else if (on_balcony && b->number[0] == 1 && b->number[1] == 3) {
        per[15] = 100; b->number[0] = 0; b->number[1] = 0;
    } else if (boss_in_region(b, BREG_BALCONY_IN1, pp) || boss_in_region(b, BREG_BALCONY_IN2, pp)) {
        per[6] = 100;
    } else if (!boss_in_region(b, BREG_ARENA, pp)) {
        per[16] = 100;
    } else if (observe(b, b->dmy_rfoot, 2)) {      /* AddObserveChrDmySphere(0, ENE_0, 3, 2) */
        per[4] = 15; per[8] = 85;
    } else if (observe(b, b->dmy_lfoot, 3)) {      /* AddObserveChrDmySphere(1, ENE_0, 4, 3) */
        per[4] = 15; per[9] = 85;
    } else if (d >= 11.5f) {
        per[1] = 70; per[2] = 5; per[5] = 10; per[7] = 10; per[11] = 5;
    } else if (d >= 7) {
        per[2] = 15; per[5] = 35; per[7] = 35; per[11] = 15;
    } else if (d >= 4.2f) {
        per[2] = 45; per[3] = 10; per[11] = 45;
    } else if (d >= 1.5f) {
        per[2] = 20; per[3] = 50; per[5] = 5; per[7] = 5; per[11] = 20;
    } else {
        per[3] = 45; per[4] = 45; per[11] = 10;
    }
    int total = 0;
    for (int i = 1; i <= 16; i++) total += per[i];
    if (total <= 0) return;
    int fate = rand_int(b, 1, total), acc = 0, n = 1; /* Common_Battle_Activate */
    for (; n <= 16; n++) { acc += per[n]; if (fate <= acc) break; }
    b->n_plan = b->plan_i = 0;
    b->well_space = act(b, n);
    b->last_act = n;
    b->think_count++;
    snprintf(b->log, sizeof(b->log), "think #%d d=%.1f ang=%.0f -> Act%02d (%d steps)", b->think_count, d,
             b->angle * 180 / PI, n, b->n_plan);
    printf("ash-boss: %s\n", b->log);
}

/* MiniGreaterDemon223200_ActAfter_AdjustSpace: after an act with GetWellSpace_Odds */
static void after_act(Boss *b, int odds)
{
    int fate_gws = rand_int(b, 1, 100);
    b->n_plan = b->plan_i = 0;
    if (fate_gws > odds) return;
    int fate = rand_int(b, 1, 100);
    if (fate > 80) {
        plan_add(b, SG_SIDEWAY, rand_int(b, 0, 1), 0, 2.5f, 1);
        snprintf(b->log, sizeof(b->log), "adjust space: sideway %s 2.5 s", b->plan[0].ez ? "right" : "left");
        printf("ash-boss: %s\n", b->log);
    }
}

/* ------------------------------------------------------------------ executing sub-goals */

static int ez_clip(const Boss *b, int ez)
{
    if (ez == 701) return b->clip_step;
    if (ez >= 3000 && ez < 3032) return b->clip_of_ez[ez - 3000];
    return -1;
}

static void turn_toward(Boss *b, float rate_deg, float dt)
{
    float step = rate_deg * PI / 180.0f * dt;
    float a = b->angle;
    float d = a > step ? step : a < -step ? -step : a;
    b->yaw = wrap_pi(b->yaw + d);
    b->angle = wrap_pi(b->angle - d);
}

static void start_ez(Boss *b, int ez, float blend)
{
    int c = ez_clip(b, ez);
    if (c < 0) { b->plan_i++; return; }
    b->ez = ez;
    b->st = BS_ACTION;
    play(b, c, blend);
}

static void next_step(Boss *b) { b->plan_i++; b->step_t = 0; }

static void step_tick(Boss *b, float dt)
{
    if (b->plan_i >= b->n_plan) {
        if (b->well_space >= 0) { int odds = b->well_space; b->well_space = -1; after_act(b, odds); }
        if (b->plan_i >= b->n_plan) { b->st = BS_IDLE; think(b); }
        if (b->plan_i >= b->n_plan) return;
    }
    BossStep *s = &b->plan[b->plan_i];
    b->step_t += dt;
    const float turn = b->hdr->turn_deg_s;
    switch (s->kind) {
    case SG_APPROACH:
    case SG_GOTO: {
        float target[3], dist;
        if (s->kind == SG_GOTO) {
            if (boss_region_center(b, s->ez == 0 ? BREG_HOME : s->ez, target) < 0) { next_step(b); return; }
            float dx = target[0] - b->pos[0], dz = target[2] - b->pos[2];
            dist = sqrtf(dx * dx + dz * dz);
            b->angle = wrap_pi(atan2f(-dx, -dz) - b->yaw);
        } else {
            dist = b->dist;
        }
        if (dist <= s->dist || b->step_t > s->life) {
            if (b->st == BS_MOVE) play(b, b->clip_idle, 1);
            b->st = BS_IDLE;
            next_step(b);
            return;
        }
        int run = !s->walk_only && dist >= s->dist + WALK_SWITCH_RUN_MARGIN;
        b->st = BS_MOVE;
        play(b, run ? b->clip_run : b->clip_walk, 1);
        turn_toward(b, turn, dt);
        return;
    }
    case SG_SIDEWAY: /* SidewayMove(2.5 s, ENE_0, right?, 15..30 deg, turnTo, walk) [версия: strafe clip] */
        if (b->step_t > s->life) { play(b, b->clip_idle, 1); b->st = BS_IDLE; next_step(b); return; }
        b->st = BS_MOVE;
        play(b, s->ez ? b->clip_walk_r : b->clip_walk_l, 1);
        turn_toward(b, turn, dt);
        return;
    case SG_WAIT:
        if (b->step_t > s->life) { next_step(b); return; }
        b->st = BS_IDLE;
        play(b, b->clip_idle, 1);
        return;
    case SG_TURN:
        if (fabsf(b->angle) < 20 * PI / 180 || b->step_t > s->life) { next_step(b); return; }
        b->st = BS_TURN;
        play(b, b->clip_idle, 1); /* [версия] turning in place on the idle pose: the 90/180 deg clips have a
                                     fixed angle and overshoot small corrections */
        turn_toward(b, turn, dt);
        return;
    case SG_ATTACK:
        /* CommonAttack: turn toward the target until within 20 deg or 1.5 s, then the EzState [версия] */
        if (b->st != BS_ACTION) {
            if (fabsf(b->angle) > 20 * PI / 180 && b->step_t < 1.5f) {
                b->st = BS_TURN;
                play(b, b->clip_idle, 1);
                turn_toward(b, turn, dt);
                return;
            }
            start_ez(b, s->ez, 1);
        }
        return;
    case SG_NONSPIN:
        if (b->st != BS_ACTION) start_ez(b, s->ez, 1);
        return;
    case SG_COMBO:
        if (b->st != BS_ACTION) start_ez(b, s->ez, 1);
        return;
    }
}

/* ------------------------------------------------------------------ tick */

static void root_motion(Boss *b, const BossWorld *w, float t0, float t1)
{
    float d[4];
    anim_root_delta(&b->anim, b->clip, t0, t1, d);
    float s = sinf(b->yaw), c = cosf(b->yaw);
    float wx = c * d[0] + s * d[2], wz = -s * d[0] + c * d[2];
    if (b->st == BS_TURN) wx = wz = 0;
    if (w && w->collide && b->st != BS_ENTRANCE) {
        w->collide(w->ctx, b->pos, wx, wz);
    } else {
        b->pos[0] += wx; b->pos[2] += wz;
        const BossFileReg *r = find_reg(b, BREG_ARENA);
        if (r && b->st != BS_ENTRANCE) { /* stay inside the hall [версия: until the map collision is wired] */
            float hx = r->size[0] * 0.5f - b->hdr->radius, hz = r->size[1] * 0.5f - b->hdr->radius;
            if (b->pos[0] < r->pos[0] - hx) b->pos[0] = r->pos[0] - hx;
            if (b->pos[0] > r->pos[0] + hx) b->pos[0] = r->pos[0] + hx;
            if (b->pos[2] < r->pos[2] - hz) b->pos[2] = r->pos[2] - hz;
            if (b->pos[2] > r->pos[2] + hz) b->pos[2] = r->pos[2] + hz;
        }
    }
    b->pos[1] += d[1];
    b->yaw = wrap_pi(b->yaw + d[3]);
}

void boss_tick(Boss *b, const BossWorld *w, float dt)
{
    if (b->hp_trail > b->hp) {
        if (b->hp_trail_wait > 0) b->hp_trail_wait -= dt;
        else b->hp_trail -= (float)b->hp_max * 0.6f * dt;
        if (b->hp_trail < b->hp) b->hp_trail = (float)b->hp;
    }
    if (b->st == BS_INACTIVE) return;
    perceive(b, w);
    float t0 = b->t;
    b->t += dt;
    b->prev_t += dt;
    b->blend += dt;

    switch (b->st) {
    case BS_WAIT:
        /* waiting at its spot until the player steps into the battle region (or comes close) */
        if (boss_in_region(b, BREG_BATTLE, w->player) || b->dist < 8) {
            b->engaged = 1;
            b->st = BS_IDLE;
            snprintf(b->log, sizeof(b->log), "engaged: player in the hall");
        }
        break;
    case BS_ENTRANCE:
        root_motion(b, w, t0, b->t);
        if (b->t >= clip_len(b, b->clip)) {
            b->st = BS_IDLE;
            play(b, b->clip_idle, 1);
            snprintf(b->log, sizeof(b->log), "landed at %.1f %.1f %.1f", b->pos[0], b->pos[1], b->pos[2]);
            printf("ash-boss: %s\n", b->log);
        }
        pose(b);
        return;
    case BS_DEATH:
        root_motion(b, w, t0, b->t);
        if (b->t >= clip_len(b, b->clip)) { b->st = BS_DEAD; b->visible = 0; printf("ash-boss: defeated\n"); }
        pose(b);
        return;
    case BS_DEAD:
        return;
    default:
        break;
    }

    if (b->st == BS_ACTION) {
        /* TAE 224: turn speed while the window is open (the demon tracks the player during wind-ups) */
        const BossFileWin *tw = window_at(b, BW_TURN, b->t * FPS);
        if (tw && !w->player_dead) turn_toward(b, (float)tw->value, dt);
        float len = clip_len(b, b->clip);
        int next_combo = b->plan_i + 1 < b->n_plan && b->plan[b->plan_i + 1].kind == SG_COMBO;
        float end = next_combo ? cancel_time(b, b->clip) : len;
        if (b->t >= end) {
            b->ez = -1;
            b->st = BS_IDLE;
            next_step(b);
            if (b->ai_mode == BOSS_AI_EXTERNAL || b->plan_i >= b->n_plan) play(b, b->clip_idle, 1);
        }
    }
    if (b->ai_mode == BOSS_AI_FSM && b->engaged && b->st != BS_ACTION && !w->player_dead) step_tick(b, dt);
    else if (w->player_dead && b->st != BS_ACTION) { play(b, b->clip_idle, 1); b->st = BS_IDLE; }
    root_motion(b, w, t0, b->t);
    pose(b);
}

/* ------------------------------------------------------------------ combat interface */

int boss_hits(const Boss *b, BossHit *out, int max)
{
    if (b->st != BS_ACTION && b->st != BS_ENTRANCE) return 0;
    int n = 0;
    float f = b->t * FPS;
    for (u32 i = 0; i < b->hdr->n_win && n < max; i++) {
        const BossFileWin *w = &b->win[i];
        if (w->clip != b->clip || w->kind != BW_HIT || f < w->f0 || f >= w->f1) continue;
        const BossFileAtk *a = &b->atk[w->value];
        for (int h = 0; h < a->n_hit && n < max; h++) {
            BossHit *o = &out[n++];
            boss_dummy_world(b, a->hit[h].dmy0, o->a);
            if (a->hit[h].dmy1 >= 0) boss_dummy_world(b, a->hit[h].dmy1, o->b);
            else { o->b[0] = o->a[0]; o->b[1] = o->a[1]; o->b[2] = o->a[2]; }
            o->radius = a->hit[h].radius;
            o->atk = w->value;
            o->swing = b->swing_base + i;
        }
    }
    return n;
}

const BossFileAtk *boss_atk(const Boss *b, int row)
{
    return row >= 0 && row < (int)b->hdr->n_atk ? &b->atk[row] : NULL;
}

void boss_hurt_capsule(const Boss *b, float a[3], float b_[3], float *radius)
{
    float r = b->hdr->radius;
    a[0] = b_[0] = b->pos[0]; a[2] = b_[2] = b->pos[2];
    a[1] = b->pos[1] + r; b_[1] = b->pos[1] + b->hdr->height - r;
    *radius = r;
}

int boss_iframes(const Boss *b) { return b->st == BS_ENTRANCE || window_at(b, BW_IFRAME, b->t * FPS) != NULL; }

int boss_take_hit(Boss *b, int damage, int poise_damage, int flags)
{
    (void)poise_damage;
    if (b->hp <= 0 || b->st == BS_INACTIVE || boss_iframes(b)) return b->hp;
    if (b->hp_trail <= b->hp) b->hp_trail_wait = 0.6f;
    b->hp -= damage;
    if (!b->engaged) { b->engaged = 1; b->st = BS_IDLE; }
    printf("ash-boss: hit %d%s -> hp %d/%d\n", damage, flags == BOSS_HIT_PLUNGE ? " (plunge)" : "", b->hp > 0 ? b->hp : 0,
           b->hp_max);
    if (b->hp <= 0) {
        b->hp = 0;
        b->st = BS_DEATH;
        b->ez = -1;
        b->n_plan = b->plan_i = 0;
        play(b, b->clip_death, 1);
        snprintf(b->log, sizeof(b->log), "death (a2200)");
        return 0;
    }
    /* Damaged_Step(ai, goal, 3, 5, 100, 0, 0, 6): 5% back step when hit within 3 m (only between actions) */
    if (b->ai_mode == BOSS_AI_FSM && b->st != BS_ACTION && b->dist <= 3 && rand_int(b, 1, 100) <= 5) {
        b->n_plan = b->plan_i = 0;
        plan_add(b, SG_NONSPIN, 701, 0, 5, 0);
        b->well_space = 0;
    }
    return b->hp;
}

void boss_event(Boss *b, int ev)
{
    if (b->ai_mode != BOSS_AI_FSM || !b->engaged || b->hp <= 0 || b->st == BS_ACTION || b->st == BS_ENTRANCE)
        return;
    if (ev == BOSS_EV_PLAYER_ATTACK) {
        /* FindAttack_Step(ai, goal, 3, 5, 100, 0, 0, 6) */
        if (b->dist <= 3 && rand_int(b, 1, 100) <= 5) {
            b->n_plan = b->plan_i = 0;
            plan_add(b, SG_NONSPIN, 701, 0, 5, 0);
            b->well_space = 0;
            snprintf(b->log, sizeof(b->log), "interrupt: FindAttack -> back step");
        }
    } else if (ev == BOSS_EV_PLAYER_ITEM) {
        /* UseItem_Act(ai, goal, 15, 15): punish estus */
        if (b->dist <= 15 && rand_int(b, 1, 100) <= 15) {
            int fate = rand_int(b, 1, 100);
            b->n_plan = b->plan_i = 0;
            if (b->dist >= 10.5f) { plan_add(b, SG_APPROACH, 0, D3007, 10, 0); plan_add(b, SG_ATTACK, 3007, 0, 10, 0); }
            else if (b->dist >= 5.2f) {
                if (fate <= 50) { plan_add(b, SG_APPROACH, 0, D3000, 10, 0); plan_add(b, SG_ATTACK, 3000, 0, 10, 0); }
                else { plan_add(b, SG_APPROACH, 0, D3002, 10, 0); plan_add(b, SG_ATTACK, 3002, 0, 10, 0); }
            } else { plan_add(b, SG_APPROACH, 0, D3006, 10, 0); plan_add(b, SG_ATTACK, 3006, 0, 10, 0); }
            b->well_space = 100;
            snprintf(b->log, sizeof(b->log), "interrupt: UseItem -> punish %d", b->plan[1].ez);
        }
    }
}

float boss_request_ez(Boss *b, int ez)
{
    int c = ez_clip(b, ez);
    if (c < 0 || b->hp <= 0) return -1;
    b->n_plan = b->plan_i = 0;
    b->ez = ez;
    b->st = BS_ACTION;
    play(b, c, 1);
    return clip_len(b, c);
}

void boss_ext_move(Boss *b, float forward, float side, int walk, int face_target, float dt)
{
    if (b->st == BS_ACTION || b->hp <= 0) return;
    int c = b->clip_idle;
    if (forward > 0.1f) c = walk ? b->clip_walk : b->clip_run;
    else if (forward < -0.1f) c = b->clip_walk; /* no back clip in use: walk_b exists but the script never backs */
    else if (side > 0.1f) c = b->clip_walk_r;
    else if (side < -0.1f) c = b->clip_walk_l;
    b->st = c == b->clip_idle ? BS_IDLE : BS_MOVE;
    play(b, c, 1);
    if (face_target) turn_toward(b, b->hdr->turn_deg_s, dt);
}

u32 boss_anim_id(const Boss *b) { return b->clip >= 0 ? b->anim.clips[b->clip].anim_id : 0; }
int boss_alive(const Boss *b) { return b->hp > 0; }
int boss_defeated(const Boss *b) { return b->st == BS_DEAD; }

void boss_lock_point(const Boss *b, float out[3])
{
    const ScePspFMatrix4 *m = &b->world[b->lock_bone];
    float p[3] = {m->w.x, m->w.y, m->w.z};
    to_world(b, p, out);
}

/* ------------------------------------------------------------------ drawing */

void boss_draw(Boss *b)
{
    if (!b->visible || b->st == BS_INACTIVE || b->st == BS_DEAD) return;
    sceGumMatrixMode(GU_MODEL);
    sceGumLoadIdentity();
    ScePspFVector3 t = {b->pos[0], b->pos[1], b->pos[2]};
    sceGumTranslate(&t);
    sceGumRotateY(b->yaw);
    chr_draw(&b->chr, b->skin);
    sceGumLoadIdentity();
}

typedef struct { u32 color; s16 x, y, z, pad; } HudV;
typedef struct { u16 u, v; s16 x, y, z; } HudT; /* 10 B: GE vertex stride, no padding */

static void rect(int x0, int y0, int x1, int y1, u32 color)
{
    HudV *v = sceGuGetMemory(2 * sizeof(HudV));
    v[0].color = color; v[0].x = (s16)x0; v[0].y = (s16)y0; v[0].z = 0;
    v[1].color = color; v[1].x = (s16)x1; v[1].y = (s16)y1; v[1].z = 0;
    sceGuDrawArray(GU_SPRITES, GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D, 2, 0, v);
}

void boss_hud_draw(const Boss *b)
{
    if (!b->engaged || b->st == BS_INACTIVE || b->st == BS_DEAD || b->st == BS_WAIT) return;
    /* DS1: name above a long thin bar at the bottom centre */
    const int x0 = 90, x1 = 390, y0 = 246, y1 = 251;
    sceGuDisable(GU_DEPTH_TEST);
    sceGuDisable(GU_TEXTURE_2D);
    sceGuEnable(GU_BLEND);
    sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
    rect(x0 - 1, y0 - 1, x1 + 1, y1 + 1, 0xc0101010);
    int w = x1 - x0;
    int tr = (int)(w * b->hp_trail / (float)b->hp_max), cur = (int)(w * b->hp / (float)b->hp_max);
    if (tr > cur) rect(x0 + cur, y0, x0 + tr, y1, 0xff2aa0c8);     /* delayed loss: yellow-orange (ABGR) */
    if (cur > 0) rect(x0, y0, x0 + cur, y1, 0xff1c1ca8);           /* HP: dark red */
    if (b->name_px) {
        sceGuEnable(GU_TEXTURE_2D);
        sceGuTexMode(GU_PSM_4444, 0, 0, 0);
        sceGuTexImage(0, b->name_w, b->name_h, b->name_w, b->name_px);
        sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
        sceGuTexFilter(GU_NEAREST, GU_NEAREST);
        sceGuTexFlush();
        HudT *v = sceGuGetMemory(2 * sizeof(HudT));
        int nx = x0, ny = y0 - b->name_h - 1;
        v[0].u = 0; v[0].v = 0; v[0].x = (s16)nx; v[0].y = (s16)ny; v[0].z = 0;
        v[1].u = (u16)b->name_tw; v[1].v = (u16)b->name_h; v[1].x = (s16)(nx + b->name_tw);
        v[1].y = (s16)(ny + b->name_h); v[1].z = 0;
        sceGuDrawArray(GU_SPRITES, GU_TEXTURE_16BIT | GU_VERTEX_16BIT | GU_TRANSFORM_2D, 2, 0, v);
        sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
        sceGuTexFilter(GU_LINEAR, GU_LINEAR);
    }
    sceGuDisable(GU_BLEND);
    sceGuEnable(GU_TEXTURE_2D);
    sceGuEnable(GU_DEPTH_TEST);
}

void boss_debug_draw(const Boss *b, const ScePspFMatrix4 *vp)
{
    BossHit h[8];
    int n = boss_hits(b, h, 8);
    sceGuDisable(GU_DEPTH_TEST);
    sceGuDisable(GU_TEXTURE_2D);
    for (int i = 0; i < n; i++) {
        for (int e = 0; e < 2; e++) {
            const float *p = e ? h[i].b : h[i].a;
            float cx = vp->x.x * p[0] + vp->y.x * p[1] + vp->z.x * p[2] + vp->w.x;
            float cy = vp->x.y * p[0] + vp->y.y * p[1] + vp->z.y * p[2] + vp->w.y;
            float cw = vp->x.w * p[0] + vp->y.w * p[1] + vp->z.w * p[2] + vp->w.w;
            if (cw < 0.1f) continue;
            int sx = (int)((cx / cw * 0.5f + 0.5f) * 480), sy = (int)((0.5f - cy / cw * 0.5f) * 272);
            int r = (int)(h[i].radius * 120.0f / cw) + 2;
            if (r > 40) r = 40;
            rect(sx - r, sy - r, sx + r, sy - r + 2, 0xff2020ff);
            rect(sx - r, sy + r - 2, sx + r, sy + r, 0xff2020ff);
            rect(sx - r, sy - r, sx - r + 2, sy + r, 0xff2020ff);
            rect(sx + r - 2, sy - r, sx + r, sy + r, 0xff2020ff);
        }
    }
    sceGuEnable(GU_TEXTURE_2D);
    sceGuEnable(GU_DEPTH_TEST);
}
