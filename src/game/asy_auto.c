/*
 * Autopilot for headless runs (PPSSPPHeadless has no pad; tests/native too): a scenario script of high-level steps
 * (go to a point, press the action button at a prompt, fight, ...) turned into a virtual pad every step. The pad
 * goes through the same input layer as a real one, so the autopilot plays by the same rules as a person:
 * stamina, i-frames, collision, doors, the AI. Nothing is teleported except where a step says WARP (scenario set-up,
 * logged) or when the walker is stuck for 6 s (logged as "STUCK ... warp", counted in the summary).
 *
 * Coordinates in the scripts are FromSoft (MSB / docs/events.md), mirrored to PSP space here.
 */
#include "asy.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "save.h"

enum {
    A_GOTO,      /* walk to x y z (arg: 1 = run) */
    A_PRESS,     /* wait for a prompt containing s, press the action button (arg: face point given) */
    A_WAIT,      /* arg seconds (fights nearby enemies meanwhile) */
    A_WAITFLAG,  /* until event flag arg is on (max 20 s) */
    A_FACE,      /* turn to x y z */
    A_KILL,      /* fight every alerted hollow within arg m until none is left */
    A_BOSS,      /* fight the Asylum Demon until it is dead (dies -> the scenario restarts from the respawn) */
    A_END,       /* until "Конец демо" (+3 s) */
    A_DRINK,     /* drink Estus */
    A_WARP,      /* set-up only: teleport (logged) */
    A_DIE,       /* stand still until dead (scenario 3) */
    A_RESPAWN,   /* wait for the reload after a death */
    A_STAIN,     /* walk to the bloodstain */
    A_REST,      /* rest at the bonfire (prompt s), leave the menu */
    A_PLUNGE,    /* walk off the ledge toward x y z, press R1 while falling */
    A_DUEL,      /* scenario 2: R1 combo, roll, block, parry + riposte against hollow `arg` (entity) */
    A_LABEL,     /* marker for retries (s = name) */
    A_RETRY,     /* if the player died since the label s: jump back to it */
    A_SAVE_SNAP, /* write the current save to s (used to start scenario 4 at bonfire 2) */
    A_LOAD_SNAP, /* scenario 4 set-up: load save s */
    A_BALL,      /* wait for the iron ball (max arg s) and roll through it (i-frames) */
    A_HEAL,      /* drink Estus until HP >= arg (fraction) or no charges */
    A_PLACE,     /* set-up only: move hollow `arg` (entity) to x y z, its new home (logged) */
    A_STOP,
};

typedef struct { int op; float x, y, z, arg; const char *s; } AStep;

/* -------------------------------------------------------------- scenario 1: the whole route, cell -> crow */
/* waypoints from tests/native/asy_path (A* over the zone collision with the game's capsule), FS coordinates */
#define ROUTE_START                                                                                       \
    {A_WAIT, 0, 0, 0, 4.0f, "opening"},                                                                  \
    {A_GOTO, -14.60f, 184.74f, -44.60f, 0, "cell"},                                                       \
    {A_GOTO, -14.00f, 184.74f, -44.50f, 0, "key"},                                                        \
    {A_FACE, -13.6f, 185.0f, -45.24f, 0, 0},                                                              \
    {A_PRESS, 0, 0, 0, 0, "Поднять"},                                                                     \
    {A_GOTO, -13.80f, 185.25f, -43.10f, 0, "cell door"},                                                  \
    {A_FACE, -13.8f, 185.2f, -42.47f, 0, 0},                                                              \
    {A_PRESS, 0, 0, 0, 0, "Открыть"},                                                                     \
    {A_WAIT, 0, 0, 0, 2.5f, 0},                                                                           \
    {A_GOTO, -13.80f, 185.25f, -41.00f, 0, "out of the cell"},                                            \
    {A_GOTO, -13.70f, 186.50f, -20.00f, 0, "corridor"},                                                   \
    {A_KILL, 0, 0, 0, 8.0f, 0},                                                                           \
    {A_GOTO, -13.50f, 190.28f, 15.50f, 0, "corridor end"},                                                \
    {A_KILL, 0, 0, 0, 8.0f, 0},                                                                           \
    {A_GOTO, -8.57f, 189.69f, 23.04f, 0, ""},                                                             \
    {A_GOTO, -0.90f, 189.69f, 22.60f, 0, "ladder"},                                                       \
    {A_PRESS, 0, 0, 0, 0, "Подняться"},                                                                   \
    {A_WAIT, 0, 0, 0, 1.5f, 0},                                                                           \
    {A_GOTO, -1.50f, 195.99f, 18.50f, 0, ""},                                                             \
    {A_GOTO, -0.50f, 195.99f, 14.00f, 0, ""},                                                             \
    {A_GOTO, 2.00f, 195.94f, 6.50f, 0, ""},                                                               \
    {A_GOTO, 3.10f, 195.95f, 6.20f, 0, "bonfire 1"},                                                      \
    {A_PRESS, 0, 0, 0, 0, "Разжечь"},                                                                     \
    {A_WAIT, 0, 0, 0, 2.0f, 0},                                                                           \
    {A_REST, 0, 0, 0, 0, "Отдохнуть"},                                                                    \
    {A_GOTO, 3.19f, 198.15f, -3.00f, 0, "big door"},                                                      \
    {A_FACE, 3.19f, 198.5f, -4.03f, 0, 0},                                                                \
    {A_PRESS, 0, 0, 0, 0, "Открыть"},                                                                     \
    {A_WAIT, 0, 0, 0, 1.5f, 0},                                                                           \
    {A_GOTO, 3.10f, 197.76f, -14.00f, 0, "hall"}

#define ROUTE_ESCAPE                                                                                      \
    {A_WAIT, 0, 0, 0, 2.0f, "the demon drops"},                                                           \
    {A_GOTO, 8.0f, 198.2f, -20.0f, 1, "run"},                                                             \
    {A_GOTO, 14.00f, 198.20f, -25.50f, 1, "gate"},                                                        \
    {A_GOTO, 22.00f, 196.33f, -25.50f, 1, "east way"},                                                    \
    {A_WAIT, 0, 0, 0, 2.0f, 0},                                                                           \
    {A_GOTO, 33.00f, 193.15f, -25.45f, 0, "bonfire 2"},                                                   \
    {A_PRESS, 0, 0, 0, 0, "Разжечь"},                                                                     \
    {A_WAIT, 0, 0, 0, 2.0f, 0},                                                                           \
    {A_SAVE_SNAP, 0, 0, 0, 0, "asy_bf2.sav"}

/* "round the corner" after "run from the ball": pushed by a firebomb the bot stood at (-7.51 13.08), and from there the
 * straight way to "out of its way" is a wall (checked with asy_path walk) */
#define ROUTE_TO_BOSS                                                                                     \
    {A_GOTO, 34.50f, 192.58f, -23.50f, 0, ""},                                                            \
    {A_GOTO, 38.54f, 192.58f, -20.59f, 0, ""},                                                            \
    {A_GOTO, 38.40f, 192.55f, -11.10f, 0, "shield"},                                                      \
    {A_PRESS, 0, 0, 0, 0, "Поднять"},                                                                     \
    {A_GOTO, 38.60f, 195.43f, 16.20f, 0, "sword"},                                                        \
    {A_PRESS, 0, 0, 0, 0, "Поднять"},                                                                     \
    {A_GOTO, 38.00f, 195.52f, 19.50f, 0, ""},                                                             \
    {A_GOTO, 6.37f, 201.78f, 19.63f, 0, "fog CP1"},                                                       \
    {A_FACE, 0.0f, 201.78f, 19.63f, 0, 0},                                                                \
    {A_PRESS, 0, 0, 0, 0, "белый свет"},                                                                  \
    {A_WAIT, 0, 0, 0, 2.5f, 0},                                                                           \
    {A_GOTO, -0.50f, 201.78f, 14.50f, 0, ""},                                                             \
    {A_GOTO, -6.50f, 201.78f, 14.50f, 0, ""},                                                             \
    {A_GOTO, -7.50f, 201.78f, 13.50f, 0, ""},                                                             \
    {A_GOTO, -8.00f, 204.21f, 7.00f, 0, "stairs: the iron ball"},                                         \
    {A_GOTO, -7.50f, 201.78f, 13.50f, 1, "run from the ball"},                                            \
    {A_GOTO, -6.50f, 201.78f, 14.50f, 1, "round the corner"},                                             \
    {A_GOTO, -1.00f, 201.78f, 14.50f, 1, "out of its way"},                                               \
    {A_BALL, 0, 0, 0, 4.0f, 0},                                                                           \
    {A_GOTO, -6.50f, 201.78f, 14.50f, 0, ""},                                                             \
    {A_GOTO, -9.00f, 201.78f, 17.50f, 0, "through the broken wall"},                                      \
    {A_GOTO, -4.59f, 202.13f, 21.97f, 0, "Oscar"},                                                        \
    {A_PRESS, 0, 0, 0, 0, "Поднять"},                                                                     \
    {A_GOTO, -9.00f, 201.78f, 17.50f, 0, ""},                                                             \
    {A_GOTO, -8.00f, 204.21f, 7.00f, 0, "stairs"},                                                        \
    {A_GOTO, -8.00f, 208.43f, -5.00f, 0, "stairs top"},                                                   \
    {A_KILL, 0, 0, 0, 8.0f, 0},                                                                           \
    {A_GOTO, -8.30f, 208.43f, -7.40f, 0, "east wing door"},                                               \
    {A_FACE, -8.6f, 208.43f, -8.65f, 0, 0},                                                               \
    {A_PRESS, 0, 0, 0, 0, "Открыть"},                                                                     \
    {A_WAIT, 0, 0, 0, 1.5f, 0},                                                                           \
    {A_KILL, 0, 0, 0, 8.0f, 0},                                                                           \
    {A_GOTO, -7.50f, 208.43f, -26.50f, 0, ""},                                                            \
    {A_GOTO, -6.00f, 208.43f, -35.00f, 0, ""},                                                            \
    {A_KILL, 0, 0, 0, 8.0f, 0},                                                                           \
    {A_GOTO, -5.51f, 208.43f, -35.51f, 0, ""},                                                            \
    {A_GOTO, -3.00f, 208.46f, -36.00f, 0, "upper corridor"},                                              \
    {A_GOTO, 1.50f, 209.41f, -36.00f, 0, ""},                                                             \
    {A_HEAL, 0, 0, 0, 0.9f, "before the boss"},                                                           \
    {A_GOTO, 3.20f, 209.71f, -35.30f, 0, "balcony fog"},                                                  \
    {A_FACE, 3.2f, 209.0f, -27.6f, 0, 0},                                                                 \
    {A_PRESS, 0, 0, 0, 0, "белый свет"},                                                                  \
    {A_PLUNGE, 3.0f, 198.0f, -27.0f, 0, "plunge"},                                                        \
    {A_BOSS, 0, 0, 0, 0, 0}

#define ROUTE_EXIT                                                                                        \
    {A_WAIT, 0, 0, 0, 3.0f, "victory"},                                                                   \
    {A_GOTO, 3.19f, 198.15f, -32.40f, 0, "back door"},                                                    \
    {A_FACE, 3.19f, 198.5f, -33.42f, 0, 0},                                                               \
    {A_PRESS, 0, 0, 0, 0, "Открыть"},                                                                     \
    {A_WAIT, 0, 0, 0, 1.5f, 0},                                                                           \
    {A_GOTO, 3.19f, 198.17f, -36.00f, 0, "out"},                                                          \
    {A_GOTO, 3.30f, 212.68f, -96.00f, 0, "the crow (cliff top)"},                                         \
    {A_END, 0, 0, 0, 0, 0}

static const AStep SC_ROUTE[] = {ROUTE_START, ROUTE_ESCAPE, {A_LABEL, 0, 0, 0, 0, "boss"}, ROUTE_TO_BOSS,
                                 ROUTE_EXIT, {A_STOP, 0, 0, 0, 0, 0}};

/* -------------------------------------------------------------- scenario 2: duel with a hollow (corridor 1810101) */
static const AStep SC_HOLLOW[] = {
    {A_WAIT, 0, 0, 0, 4.0f, "opening"},
    {A_PLACE, 6.0f, 196.0f, 10.0f, 1810103, "sword hollow 1810103 into the open courtyard"},
    {A_WARP, 6.0f, 196.0f, 14.5f, 0, "courtyard, 4.5 m from it"},
    {A_DUEL, 0, 0, 0, 1810103, 0},
    {A_KILL, 0, 0, 0, 10.0f, 0},
    {A_WAIT, 0, 0, 0, 2.0f, 0},
    {A_STOP, 0, 0, 0, 0, 0},
};

/* -------------------------------------------------------------- scenario 3: death -> bonfire -> bloodstain */
static const AStep SC_DEATH[] = {
    ROUTE_START,
    {A_WAIT, 0, 0, 0, 1.0f, "the demon drops"},
    {A_DIE, 0, 0, 0, 0, "stand in the hall"},
    {A_RESPAWN, 0, 0, 0, 0, 0},
    {A_WAIT, 0, 0, 0, 1.0f, 0},
    {A_GOTO, 3.19f, 198.15f, -3.00f, 0, "respawned at bonfire 1: back to the big door"},                 \
    {A_FACE, 3.19f, 198.5f, -4.03f, 0, 0},                                                                \
    {A_PRESS, 0, 0, 0, 0, "Открыть"},                                                                     \
    {A_WAIT, 0, 0, 0, 1.5f, 0},                                                                           \
    {A_STAIN, 0, 0, 0, 0, "bloodstain"},
    ROUTE_ESCAPE,
    {A_WAIT, 0, 0, 0, 2.0f, 0},
    {A_STOP, 0, 0, 0, 0, 0},
};

/* -------------------------------------------------------------- scenario 4: the boss (starts from bonfire 2) */
/* the snapshot loaded: straight to the label "boss" at bonfire 2. No snapshot (a fresh stage / memory stick): the
 * route up to bonfire 2 first (it lights it and writes the snapshot), so the fight and every respawn after a death
 * are at bonfire 2 too. Before, a missing snapshot left the knight in the cell: ROUTE_TO_BOSS got STUCK 51 m from its
 * first point -> warp past the escape (the demon's drop event never ran), and each death respawned in the cell -> the
 * same warp again (night-r1/B-boss: asy_warps 4, deaths 3) [проверено: summary.log] */
static const AStep SC_BOSS[] = {
    {A_LOAD_SNAP, 0, 0, 0, 0, "asy_bf2.sav"},
    ROUTE_START,
    ROUTE_ESCAPE,
    {A_LABEL, 0, 0, 0, 0, "boss"},
    {A_WAIT, 0, 0, 0, 1.0f, 0},
    ROUTE_TO_BOSS,
    ROUTE_EXIT,
    {A_STOP, 0, 0, 0, 0, 0},
};

static const AStep *const SCRIPTS[ASY_SC_COUNT] = {NULL, SC_ROUTE, SC_HOLLOW, SC_DEATH, SC_BOSS};
static const char *const SC_NAMES[ASY_SC_COUNT] = {"none", "route", "hollow", "death", "boss"};

const char *auto_scenario_name(int sc) { return sc >= 0 && sc < ASY_SC_COUNT ? SC_NAMES[sc] : "?"; }

void auto_init(Game *g, int scenario)
{
    memset(&g->au, 0, sizeof g->au);
    g->au.scenario = scenario;
    g->au.fight_target = -1;
    g->au.ignore_en = -1;
    g->au.best_d = 1e9f;
}

/* -------------------------------------------------------------- pad helpers */

static u32 clip_anim_id_of(const Enemy *e)
{
    return e->body.cur.set && e->body.cur.idx >= 0 ? e->body.cur.set->clips[e->body.cur.idx].anim_id : 0;
}

typedef struct { float sx, sy; u32 buttons; } Pad;

/* how far a roll carries the knight: the roll from the stairs (route, f5027) went 3.2 m, the old 2.4 m floor check
 * passed and the knight rolled off the side of the stairs into the courtyard 6.7 m below [проверено: native trace] */
#define ROLL_REACH 3.6f

/* is there floor (no drop > 1.2 m) `ahead` metres from the player in world direction (dx, dz)? */
static int floor_ahead(const Game *g, float dx, float dz, float ahead)
{
    float l = sqrtf(dx * dx + dz * dz), h;
    if (l < 1e-4f || !g->has_col) return 1;
    for (float a = 0.6f; a <= ahead + 1e-3f; a += 0.6f) {
        float x = g->pl.pos[0] + dx / l * a, z = g->pl.pos[2] + dz / l * a;
        if (pcol_floor((Pcol *)&g->C, x, z, g->pl.pos[1] + 2.0f, g->pl.pos[1] - 1.4f, 0.64f, &h) < 0 &&
            !world_obj_floor(&g->W, x, z, g->pl.pos[1] + 2.0f, g->pl.pos[1] - 1.4f, &h))
            return 0;
    }
    return 1;
}

/* can the knight walk straight to p (no closed door / bars / gate / fog between, same floor)? */
static int reachable(const Game *g, const float p[3])
{
    if (fabsf(p[1] - g->pl.pos[1]) > 2.0f) return 0;
    float dx = p[0] - g->pl.pos[0], dz = p[2] - g->pl.pos[2], l = sqrtf(dx * dx + dz * dz);
    for (float a = 0.3f; a < l - 0.5f; a += 0.3f) {
        float q[3] = {g->pl.pos[0] + dx / l * a, g->pl.pos[1], g->pl.pos[2] + dz / l * a}, q0[3];
        memcpy(q0, q, 12);
        if (ev_push_out(&g->ev, q, 0.2f, 0)) return 0;
        if (world_obj_push((World *)&g->W, q, 0.2f, 1.6f)) return 0;
        (void)q0;
    }
    return 1;
}


/* stick toward a world point (camera-relative, as a person holds it) */
static void stick_to(const Game *g, const float tgt[3], float mag, Pad *o)
{
    float dx = tgt[0] - g->pl.pos[0], dz = tgt[2] - g->pl.pos[2], l = sqrtf(dx * dx + dz * dz);
    if (l < 1e-4f) return;
    dx /= l; dz /= l;
    /* the player maps the stick with the camera yaw frozen at deflection start; with a lock: the target direction */
    float cy = g->lock >= 0 ? atan2f(-(g->lock_pt[0] - g->pl.pos[0]), -(g->lock_pt[2] - g->pl.pos[2]))
                            : (g->pl.basis_valid ? g->pl.basis_yaw : g->cam.yaw);
    float fx = -sinf(cy), fz = -cosf(cy), rx = cosf(cy), rz = -sinf(cy);
    float f = dx * fx + dz * fz, r = dx * rx + dz * rz;
    o->sx = r * mag;
    o->sy = -f * mag;
}

/* away from `from` (side: +1 left, -1 right, 0 straight back); tries the other ways if there is a drop behind */
static int stick_away(const Game *g, const float from[3], float side, Pad *o)
{
    float dx = g->pl.pos[0] - from[0], dz = g->pl.pos[2] - from[2], l = sqrtf(dx * dx + dz * dz);
    if (l < 1e-3f) { dx = 0; dz = 1; l = 1; }
    dx /= l; dz /= l;
    static const float SIDES[5][2] = {{1, 0}, {0.7f, 0.7f}, {0.7f, -0.7f}, {0, 1}, {0, -1}};   /* (back, side) */
    for (int k = 0; k < 5; k++) {
        float b = SIDES[k][0], sd = SIDES[k][1] + (k == 0 ? side : 0);
        float wx = dx * b + (-dz) * sd, wz = dz * b + dx * sd;
        if (!floor_ahead(g, wx, wz, 2.4f)) continue;
        float t[3] = {g->pl.pos[0] + wx * 2, g->pl.pos[1], g->pl.pos[2] + wz * 2};
        stick_to(g, t, 1.0f, o);
        return 1;
    }
    return 0;   /* nowhere safe: stay */
}

static int target_alive(const Game *g, int t, float pos[3])
{
    if (t == 100) {
        if (!g->has_boss || !boss_alive(&g->B) || g->B.st == BS_INACTIVE || g->boss_hidden) return 0;
        memcpy(pos, g->B.pos, 12);
        return 1;
    }
    if (t < 0 || t >= g->n_en) return 0;
    const Enemy *e = &g->en[t];
    if (!e->enabled || e->dead || e->dying) return 0;
    memcpy(pos, e->pos, 12);
    return 1;
}

static int nearest_enemy(const Game *g, float radius)
{
    int bi = -1;
    float bd = radius;
    for (int i = 0; i < g->n_en; i++) {
        const Enemy *e = &g->en[i];
        if (!e->enabled || e->dead || e->dying || !e->alerted) continue;
        if (i == g->au.ignore_en && g->time < g->au.ignore_until) continue;
        if (fabsf(e->pos[1] - g->pl.pos[1]) > 3.0f) continue;
        float d = asy_flat_dist(e->pos, g->pl.pos);
        if (d < bd && reachable(g, e->pos)) { bd = d; bi = i; }
    }
    return bi;
}

/* a hollow the knight cannot get to (the archer on its ledge, behind a railing: "reachable" sees no door, but the
 * capsule is stopped) made the bot jog on the spot for 11 minutes (scenario boss, f2580-f22407) [проверено: native
 * trace]. No approach progress for 5 s while out of reach: leave it alone for 30 s, as a person would walk on. */
static int fight_stalled(Game *g, int t)
{
    Auto *au = &g->au;
    if (t < 0 || t >= g->n_en) return 0;
    float d = asy_flat_dist(g->en[t].pos, g->pl.pos);
    if (t != au->fight_target) { au->fight_target = t; au->appr_best = d; au->appr_t = 0; return 0; }
    if (d < 2.2f || d < au->appr_best - 0.2f) { if (d < au->appr_best) au->appr_best = d; au->appr_t = 0; return 0; }
    au->appr_t += ASY_DT;
    if (au->appr_t < 5.0f) return 0;
    asy_log(g, "auto: hollow %u unreachable (%.1f m, %.1f m higher), left alone for 30 s", (unsigned)g->en[t].entity, d,
            g->en[t].pos[1] - g->pl.pos[1]);
    au->ignore_en = t;
    au->ignore_until = g->time + 30.0f;
    au->fight_target = -1;
    return 1;
}

static int target_attacking(const Game *g, int t)
{
    if (t == 100) return g->B.st == BS_ACTION;
    return g->en[t].ez >= 0;
}

static int player_free(const Game *g)
{
    PSt s = g->pl.st;
    return s <= P_RUN || s == P_GUARD;
}

/* The target's current swing: seconds until its next hit window (0 = active now) and seconds since its last one
 * ended (punish window). Hollows: TAE windows of the combat data; the demon: its .ashboss windows. 0 = no swing. */
static int swing_info(const Game *g, int t, float *until, float *since_end)
{
    *until = 99;
    *since_end = -1;
    if (t == 100) {
        const Boss *b = &g->B;
        if (b->st != BS_ACTION && b->st != BS_ENTRANCE) return 0;
        float f = b->t * 30.0f, last_end = -1;
        int any = 0;
        for (u32 i = 0; i < b->hdr->n_win; i++) {
            const BossFileWin *w = &b->win[i];
            if (w->clip != b->clip || w->kind != BW_HIT) continue;
            any = 1;
            if (f >= w->f0 && f < w->f1) { *until = 0; return 1; }
            if (w->f0 > f && (w->f0 - f) / 30.0f < *until) *until = (w->f0 - f) / 30.0f;
            if (w->f1 <= f && w->f1 > last_end) last_end = w->f1;
        }
        if (*until >= 99 && last_end >= 0) *since_end = (f - last_end) / 30.0f;
        return any;
    }
    const Enemy *e = &g->en[t];
    if (e->ez < 0) return 0;
    const CbAnim *an = cb_anim_find(&g->cbd, e->act.kind, clip_anim_id_of(e));
    if (!an || !an->n_hit) return 0;
    float f = e->body.t * 30.0f, last_end = -1;
    for (int i = 0; i < an->n_hit; i++) {
        const CbHit *h = &g->cbd.hit[an->hit_first + i];
        if (f >= h->start && f < h->end) { *until = 0; return 1; }
        if (h->start > f && (h->start - f) / 30.0f < *until) *until = (h->start - f) / 30.0f;
        if (h->end <= f && h->end > last_end) last_end = h->end;
    }
    if (*until >= 99 && last_end >= 0) *since_end = (f - last_end) / 30.0f;
    return 1;
}

/* one step of fighting `t` (hollow index or 100 = the demon), as a careful DS1 player:
 *   - a swing is about to land and we are in reach: roll sideways through it (i-frames), or raise the shield;
 *   - in reach and it is not swinging (or just started: our R1 lands first, or it is recovering): R1;
 *   - the demon winding up: keep out of reach; low HP and nothing coming: back off and drink Estus;
 *   - else close in. Never rolls / backs off over a drop (floor_ahead). */
static int g_fighting;

static void fight(Game *g, int t, Pad *o)
{
    Auto *au = &g->au;
    g_fighting = 1;
    float tp[3];
    if (!target_alive(g, t, tp)) return;
    int boss = t == 100;
    float d = asy_flat_dist(tp, g->pl.pos);
    if (boss) d -= ASY_DEMON_BODY_R;
    if (g->lock != t && ((int)g->frame % 15) == 0) o->buttons |= PSP_CTRL_LTRIGGER;
    float sp = g->pact.sp, hp = g->pact.hp / g->pact.hp_max;
    au->fight_t += ASY_DT;
    float until, since;
    int swing = swing_info(g, t, &until, &since);
    float reach = boss ? 5.5f : 2.6f;      /* how far its swings reach [версия: by eye from the hit volumes] */
    int threat = swing && until < 0.5f && d < reach;
    int free = player_free(g);
    /* keep pressing the dodge direction for the roll that was just tapped */
    if (au->phase == 1) {
        float side[3] = {au->x, g->pl.pos[1], au->z};
        stick_to(g, side, 1.0f, o);
        au->phase = 0;
        return;
    }
    if (threat && free && sp > 8 && until < (boss ? 0.2f : 0.28f)) {
        /* roll sideways (to the attacker's left, alternating), away from drops */
        float dx = g->pl.pos[0] - tp[0], dz = g->pl.pos[2] - tp[2], l = sqrtf(dx * dx + dz * dz) + 1e-4f;
        float sgn = ((g->frame / 90) & 1) ? 1.0f : -1.0f;
        float wx = -dz / l * sgn + (boss ? 0 : dx / l * 0.3f), wz = dx / l * sgn + (boss ? 0 : dz / l * 0.3f);
        if (boss) {   /* the demon: roll forward-diagonal, under its arm toward its flank (stay close for the punish) */
            const Boss *b = &g->B;
            float fx = -sinf(b->yaw), fz = -cosf(b->yaw), sx = -fz, sz = fx;
            float side = (dx * sx + dz * sz) >= 0 ? 1.0f : -1.0f;
            float gx = b->pos[0] + sx * side * (ASY_DEMON_BODY_R + 0.6f) - fx * 0.8f - g->pl.pos[0];
            float gz = b->pos[2] + sz * side * (ASY_DEMON_BODY_R + 0.6f) - fz * 0.8f - g->pl.pos[2];
            float gl = sqrtf(gx * gx + gz * gz) + 1e-4f;
            if (l < ASY_DEMON_BODY_R + 4.0f) { wx = gx / gl; wz = gz / gl; }
        }
        if (!floor_ahead(g, wx, wz, ROLL_REACH)) { wx = -wx; wz = -wz; }
        if (floor_ahead(g, wx, wz, ROLL_REACH)) {
            au->x = g->pl.pos[0] + wx * 2; au->z = g->pl.pos[2] + wz * 2;
            float side[3] = {au->x, g->pl.pos[1], au->z};
            stick_to(g, side, 1.0f, o);
            o->buttons |= PSP_CTRL_CROSS;
            au->phase = 1;
            if (g->verbose) asy_log(g, "auto: roll from a swing (%.2f s before the hit)", until);
            return;
        }
        o->buttons |= PSP_CTRL_RTRIGGER;   /* nowhere to roll: shield */
        return;
    }
    if (threat && !boss) { o->buttons |= PSP_CTRL_RTRIGGER; return; }   /* hollow winding up: shield up */
    float bfront = 0;
    if (boss) {
        float rx = g->pl.pos[0] - g->B.pos[0], rz = g->pl.pos[2] - g->B.pos[2], rl = sqrtf(rx * rx + rz * rz) + 1e-4f;
        bfront = (rx * -sinf(g->B.yaw) + rz * -cosf(g->B.yaw)) / rl;
    }
    if (boss && swing && until < 1.0f && d < reach && bfront > 0.4f) {  /* winding up in front of it: get out of reach */
        if (!stick_away(g, tp, 1.0f, o)) o->buttons |= PSP_CTRL_RTRIGGER;
        return;
    }
    if (hp < 0.5f && asy_has_estus(g) && g->prg.estus > 0) {
        int urgent = hp < 0.3f && !threat && !(swing && until < 1.6f);   /* drink even close when nothing is coming */
        if (urgent || (d > (boss ? 5.0f : 4.0f) && !threat && !(boss && swing && until < 2.5f))) {
            if (free && ((int)g->frame % 10) == 0) o->buttons |= PSP_CTRL_CIRCLE;
        } else {
            stick_away(g, tp, 0.0f, o);
        }
        return;
    }
    if (boss) {
        /* the demon: go for its legs from the side (not its front), hit in its recoveries */
        const Boss *b = &g->B;
        float fx = -sinf(b->yaw), fz = -cosf(b->yaw), r = ASY_DEMON_BODY_R;
        float rx = g->pl.pos[0] - b->pos[0], rz = g->pl.pos[2] - b->pos[2], rl = sqrtf(rx * rx + rz * rz) + 1e-4f;
        float front = (rx * fx + rz * fz) / rl;
        float sx = -fz, sz = fx, sgn = (rx * sx + rz * sz) >= 0 ? 1.0f : -1.0f;
        /* its legs are at +-1.5 m to the sides (hurt capsules of the combat data): stand at its flank */
        float goal[3] = {b->pos[0] + (sx * sgn * 0.95f - fx * 0.3f) * (r + 0.4f), g->pl.pos[1],
                         b->pos[2] + (sz * sgn * 0.95f - fz * 0.3f) * (r + 0.4f)};
        float dg = asy_flat_dist(goal, g->pl.pos);
        int opening = !swing || since >= 0 || until > 0.9f;   /* R1 lands 0.53 s after the press */
        if (g->verbose && (g->frame % 15) == 0)
            asy_log(g, "auto: boss: d %.1f front %.2f swing %d until %.2f since %.2f open %d sp %.0f st %s bst %d ez %d",
                    d, front, swing, until, since, opening, sp, asy_pst_name[g->pl.st], (int)b->st, b->ez);
        if ((dg < 1.0f || d < 0.7f) && opening) {
            /* R1, and chain the next R1 of the combo while the recovery lasts (since >= 0: >1 s left) */
            int chain = g->pl.st == P_ATTACK && g->pb.t > 0.35f && (since >= 0 || !swing) && sp > 25;
            if (sp > 20 && (free || chain) && ((int)g->frame % 4) == 0) o->buttons |= PSP_CTRL_SQUARE;
            return;
        }
        if (d < 1.2f && dg > 1.0f) {   /* pressed against its body: circle round it toward the flank */
            float tx = -rz / rl, tz = rx / rl;                 /* tangent, counter-clockwise */
            float gx = goal[0] - g->pl.pos[0], gz = goal[2] - g->pl.pos[2];
            if (tx * gx + tz * gz < 0) { tx = -tx; tz = -tz; }
            float t2[3] = {g->pl.pos[0] + tx * 2 + rx / rl * 0.6f, g->pl.pos[1], g->pl.pos[2] + tz * 2 + rz / rl * 0.6f};
            stick_to(g, t2, 1.0f, o);
            return;
        }
        stick_to(g, d > 3.5f ? tp : goal, 1.0f, o);
        if (d > 6.0f) o->buttons |= PSP_CTRL_CROSS;
        return;
    }
    float in_range = 1.5f;
    if (d > in_range) {
        stick_to(g, tp, 1.0f, o);
        if (d > 6.0f) o->buttons |= PSP_CTRL_CROSS;   /* run */
        return;
    }
    int opening = !swing || (since >= 0) || (!boss && until > 0.6f);
    if (opening && sp > 20 && free && ((int)g->frame % 4) == 0) o->buttons |= PSP_CTRL_SQUARE;
    else if (sp <= 20 && !boss) o->buttons |= PSP_CTRL_RTRIGGER;   /* catch the breath behind the shield */
    else if (sp <= 20) stick_away(g, tp, 0.0f, o);
}

/* -------------------------------------------------------------- the script */

static void next(Game *g, const char *why)
{
    Auto *au = &g->au;
    const AStep *st = &SCRIPTS[au->scenario][au->step];
    if (why) asy_log(g, "auto: step %d %s done (%s) at (%.2f %.2f %.2f) t=%.1fs", au->step,
                     st->s ? st->s : "", why, g->pl.pos[0], g->pl.pos[1], g->pl.pos[2], au->step_t);
    au->step++;
    au->step_t = 0;
    au->stuck_t = 0;
    au->best_d = 1e9f;
    au->phase = 0;
    au->prompt_tries = 0;
    au->fight_target = -1;
    au->fight_t = 0;
}

static void warp(Game *g, float x, float y, float z, const char *why)
{
    float p[3];
    asy_fs(x, y, z, p);
    asy_log(g, "auto: WARP (%s) (%.2f %.2f %.2f) -> (%.2f %.2f %.2f)", why, g->pl.pos[0], g->pl.pos[1], g->pl.pos[2],
            p[0], p[1], p[2]);
    memcpy(g->pl.pos, p, 12);
    g->pl.vy = 0;
    if (g->has_col) {
        float h;
        int t = pcol_floor(&g->C, p[0], p[2], p[1] + 1.5f, p[1] - 4.0f, 0.64f, &h);
        if (t >= 0) { g->pl.pos[1] = h; g->pl.floor_tri = t; }
    }
    cam_reset(&g->cam, &g->ccfg, g->row, g->pl.pos, g->pl.yaw);
}

static int find_label(const AStep *s, const char *name)
{
    for (int i = 0; s[i].op != A_STOP; i++)
        if (s[i].op == A_LABEL && s[i].s && !strcmp(s[i].s, name)) return i;
    return -1;
}

static int g_deaths_at_label;


void auto_pad(Game *g, RawPad *out)
{
    Auto *au = &g->au;
    Pad o = {0, 0, 0};
    g_fighting = 0;
    out->buttons = 0;
    out->lx = out->ly = 128;
    if (au->done || au->scenario <= 0) return;
    const AStep *script = SCRIPTS[au->scenario];
    const AStep *st = &script[au->step];
    au->step_t += ASY_DT;
    Player *p = &g->pl;
    float tgt[3];
    asy_fs(st->x, st->y, st->z, tgt);

    if (au->note[0] == 'R') {           /* waiting for the reload after a death */
        if (p->dead || prg_locked(&g->prg)) goto emit;
        au->note[0] = 0;
        au->stuck_t = 0;
        au->best_d = 1e9f;
        asy_log(g, "auto: respawned at (%.2f %.2f %.2f), continuing at step %d", p->pos[0], p->pos[1], p->pos[2], au->step);
    }
    /* a death outside of the steps that expect it: wait for the reload, then retry from the last label */
    if (p->dead && st->op != A_DIE && st->op != A_RESPAWN) {
        int lb = -1;
        for (int i = au->step; i >= 0; i--) if (script[i].op == A_LABEL) { lb = i; break; }
        asy_log(g, "auto: died at step %d -> wait for the respawn, retry from %s", au->step, lb >= 0 ? script[lb].s : "here");
        au->step = lb >= 0 ? lb : au->step;
        au->step_t = 0;
        au->phase = 0;
        au->boss_attempts++;
        au->note[0] = 'R';
        goto emit;
    }
    /* a fall off the route (a drop > 2.5 m outside the plunge and the boss fight): the bot cannot find its way back
     * (from the courtyard under the ball stairs the way up runs through the demon's hall and the closed gate), so it
     * goes back to the last waypoint below the current step: a warp, logged as "FELL" and counted in warps/falls */
    if (!p->grounded || p->st == P_FALL) au->was_air = 1;
    else {
        if (au->was_air && g->has_col && st->op != A_PLUNGE && st->op != A_BOSS) {
            float drop = au->ground[1] - p->pos[1];
            int wp = -1;
            for (int i = au->step - 1; i >= 0 && wp < 0; i--) if (script[i].op == A_GOTO) wp = i;
            if (drop > 2.5f && wp >= 0 && script[wp].y > p->pos[1] + 2.0f) {
                au->falls++;
                au->warps++;
                asy_log(g, "auto: FELL %.1f m off the route at step %d (from %.2f %.2f %.2f) -> back to waypoint %d", drop,
                        au->step, au->ground[0], au->ground[1], au->ground[2], wp);
                warp(g, script[wp].x, script[wp].y, script[wp].z, "fell");
                au->stuck_t = 0;
                au->best_d = 1e9f;
            }
        }
        au->was_air = 0;
        memcpy(au->ground, p->pos, 12);
    }
    /* bonfire menu open (rest): leave it */
    if (g->prg.phase == PRG_REST && g->prg.sub == 1) {
        if (((int)g->frame % 20) == 0) {
            int last = g->prg.menu_n - 1;
            if (g->prg.menu_cursor < last) o.buttons |= PSP_CTRL_DOWN;
            else o.buttons |= PSP_CTRL_CROSS;
        }
        goto emit;
    }
    if (prg_locked(&g->prg) || p->st == P_FOG || p->st == P_LADDER) goto emit;

    /* enemies interrupt walking: fight whatever attacks within 4 m */
    if ((st->op == A_GOTO || st->op == A_WAIT || st->op == A_PRESS) && au->scenario != ASY_SC_DEATH + 100) {
        int e = nearest_enemy(g, 4.0f);
        if (e >= 0 && !fight_stalled(g, e)) { fight(g, e, &o); au->stuck_t = 0; au->best_d = 1e9f; goto emit; }   /* a fight moves the bot: measure progress anew */
        if (g->lock >= 0 && g->lock != 100 && ((int)g->frame % 15) == 0) { o.buttons |= PSP_CTRL_LTRIGGER; }
    }

    switch (st->op) {
    case A_GOTO: {
        float d = asy_flat_dist(tgt, p->pos), dy = fabsf(tgt[1] - p->pos[1]);
        /* reached = on the point's floor too (right under the stairs top 12 m below is not there) */
        if (d < 0.45f && dy < 1.5f) { next(g, "reached"); break; }
        float d3 = sqrtf(d * d + dy * dy);   /* progress in 3D: walking under a ledge is no progress */
        if (g->lock >= 0 && ((int)g->frame % 15) == 0) o.buttons |= PSP_CTRL_LTRIGGER;   /* unlock to walk */
        stick_to(g, tgt, d < 1.2f ? 0.5f : 1.0f, &o);
        if (au->stuck_t > 1.5f && d > 0.6f) {
            /* no progress for 1.5 s (a corner, a railing): slide sideways along it, as a person would, sides alternating
               every 1.5 s, never toward a drop; the warp below stays the last resort */
            float dx = (tgt[0] - p->pos[0]) / d, dz = (tgt[2] - p->pos[2]) / d;
            float sd = ((int)(au->stuck_t / 1.5f) & 1) ? 1.0f : -1.0f;
            for (int k = 0; k < 2; k++, sd = -sd) {
                float wx = -dz * sd + dx * 0.3f, wz = dx * sd + dz * 0.3f;
                if (!floor_ahead(g, wx, wz, 1.2f)) continue;
                float side[3] = {p->pos[0] + wx * 2, p->pos[1], p->pos[2] + wz * 2};
                stick_to(g, side, 1.0f, &o);
                break;
            }
        }
        if (st->arg > 0 && d > 2.0f) o.buttons |= PSP_CTRL_CROSS;
        if (d3 < au->best_d - 0.2f) { au->best_d = d3; au->stuck_t = 0; }
        else au->stuck_t += ASY_DT;
        if (au->stuck_t > 6.0f) {
            au->stuck_total++;
            asy_log(g, "auto: STUCK %.1f m from %s (%.2f %.2f %.2f), %.1f m flat, %.1f m height -> warp", d3,
                    st->s ? st->s : "point", tgt[0], tgt[1], tgt[2], d, tgt[1] - p->pos[1]);
            au->warps++;
            warp(g, st->x, st->y, st->z, "stuck");
            next(g, "warped");
        }
        break;
    }
    case A_FACE:
        p->yaw = atan2f(-(tgt[0] - p->pos[0]), -(tgt[2] - p->pos[2]));   /* a tap of the stick toward it */
        next(g, NULL);
        break;
    case A_PRESS: {
        const char *pr = ev_prompt(&g->ev);
        if (au->phase == 1) { au->phase = 0; next(g, "pressed"); }
        else if (pr && (!st->s || strstr(pr, st->s))) {
            o.buttons |= PSP_CTRL_UP; au->phase = 1; asy_log(g, "auto: press [%s]", pr);
        } else if (au->step_t > 3.0f) {
            asy_log(g, "auto: no prompt '%s' (have '%s')", st->s ? st->s : "*", pr ? pr : "-");
            next(g, "no prompt");
        }
        break;
    }
    case A_WAIT:
        if (au->step_t >= st->arg) next(g, NULL);
        break;
    case A_WAITFLAG:
        if (ev_flag(&g->ev, (u32)st->arg) || au->step_t > 20.0f) next(g, "flag");
        break;
    case A_KILL: {
        int e = nearest_enemy(g, st->arg);
        if (e < 0) { next(g, au->fight_t > 0 ? "area clear" : NULL); break; }
        if (fight_stalled(g, e)) break;
        fight(g, e, &o);
        if (au->step_t > 60.0f) next(g, "timeout");
        break;
    }
    case A_BOSS:
        if (!g->has_boss || boss_defeated(&g->B)) { next(g, "DEMON DEFEATED"); break; }
        if (!boss_alive(&g->B)) break;
        if (g->doorfight && au->step_t < 60.0f) {
            /* regression test (opt "doorfight"): stand still in front of the closed back door 1811110 and let the
               Demon's swings and body shove the knight into it; he must stay on the arena side (z > -33.42) */
            float door[3];
            asy_fs(3.19f, 198.15f, -32.60f, door);
            int ret = 0;
            if (au->phase == 0) { au->phase = 1; au->x = 0; au->z = 0; ret = 1; }
            /* back to the door whenever the swings carried him off (each return = another try) */
            if (asy_flat_dist(p->pos, door) > 1.5f && p->st != P_REACT && (g->frame % 15) == 0) ret = 1;
            if (ret) {
                warp(g, 3.19f, 198.15f, -32.60f, "doorfight");
                au->z += 1;
                /* the Demon's body 1 m in front (arena side): its push-out (r 2.05 m) shoves the knight 1 m toward
                   the door in one step - the jump that carried him through the 0.2 m leaf before */
                g->B.pos[0] = p->pos[0];
                g->B.pos[2] = p->pos[2] + 1.0f;
            }
            g->pact.hp = g->pact.hp_max;
            if (p->pos[2] < au->x) au->x = p->pos[2];
            if (p->pos[2] < -33.45f && (g->frame % 30) == 0) asy_log(g, "doorfight: BEHIND THE DOOR z %.2f", p->pos[2]);
            if ((g->frame % 30) == 0)
                asy_log(g, "doorfight: pos (%.2f %.2f %.2f) st %d min z %.2f returns %d", p->pos[0], p->pos[1], p->pos[2], p->st,
                        au->x, (int)au->z);
            break;
        }
        fight(g, 100, &o);
        if (au->step_t > 300.0f) { asy_log(g, "auto: boss fight timeout"); au->done = 1; }
        break;
    case A_END:
        if (g->end_demo && g->end_t > 3.0f) { next(g, "END"); }
        else if (au->step_t > 30.0f) next(g, "no end event");
        break;
    case A_DRINK:
        if (au->phase == 0) { o.buttons |= PSP_CTRL_CIRCLE; au->phase = 1; }
        else if (au->step_t > 2.5f) next(g, NULL);
        break;
    case A_WARP:
        warp(g, st->x, st->y, st->z, st->s ? st->s : "set-up");
        next(g, NULL);
        break;
    case A_DIE:
        if (au->phase == 0) { au->phase = 1; au->x = (float)g->deaths; }
        if (p->dead || g->deaths > (int)au->x) next(g, "dead");
        else if (au->step_t > 90.0f) { asy_log(g, "auto: did not die in 90 s"); next(g, "timeout"); }
        break;
    case A_RESPAWN:
        if (!p->dead && !prg_locked(&g->prg) && g->reloads > 0) next(g, "respawned");
        break;
    case A_STAIN: {
        if (!g->prg.stain_on) { next(g, "souls back"); break; }
        float d = asy_flat_dist(g->prg.stain_pos, p->pos);
        stick_to(g, g->prg.stain_pos, d < 1.0f ? 0.4f : 1.0f, &o);
        if (au->step_t > 40.0f) next(g, "timeout");
        break;
    }
    case A_REST:
        if (au->phase == 0) {
            const char *pr = ev_prompt(&g->ev);
            if (pr && st->s && strstr(pr, st->s)) { o.buttons |= PSP_CTRL_UP; au->phase = 1; asy_log(g, "auto: rest [%s]", pr); }
            else if (au->step_t > 3.0f) next(g, "no rest prompt");
        } else if (au->phase == 1 && g->prg.phase == PRG_PLAY && au->step_t > 1.0f) next(g, "rested");
        else if (au->step_t > 25.0f) next(g, "rest timeout");
        break;
    case A_PLUNGE:
        /* through the balcony fog (7410 walk), then jog off the edge toward the demon and R1 while falling */
        if (au->phase == 0) {
            if (p->st == P_FOG) break;
            if (p->st == P_FALL) { au->phase = 1; break; }
            stick_to(g, tgt, 0.8f, &o);
            if (au->step_t > 10.0f) { asy_log(g, "auto: did not fall from the balcony"); next(g, "no fall"); }
        } else if (au->phase == 1) {
            o.buttons |= PSP_CTRL_SQUARE;
            au->phase = 2;
        } else if (p->grounded && p->st != P_PLUNGE && p->st != P_FALL) {
            next(g, g->plunges ? "plunge landed" : "landed, no plunge hit");
        }
        break;
    case A_DUEL: {
        /* scripted showcase against one hollow: lock, R1 combo, roll out, block a swing, parry + riposte */
        Enemy *e = NULL;
        int ei = -1;
        for (int i = 0; i < g->n_en; i++) if (g->en[i].entity == (u32)st->arg) { e = &g->en[i]; ei = i; }
        if (!e || e->dead || e->dying) { next(g, "hollow dead"); break; }
        if (!e->alerted) { e->alerted = 1; if (e->ai_ok) e->ai.battle_state = 1; }
        float d = asy_flat_dist(e->pos, p->pos);
        if (g->lock != ei && ((int)g->frame % 15) == 0) o.buttons |= PSP_CTRL_LTRIGGER;
        switch (au->phase) {
        case 0:   /* approach */
            if (d > 1.6f) stick_to(g, e->pos, 1.0f, &o);
            else { au->phase = 1; au->x = 0; asy_log(g, "auto: duel: R1 combo"); }
            break;
        case 1:   /* one R1 (a second one would kill it before the parry) */
            if (player_free(g) && au->x < 1 && ((int)g->frame % 4) == 0) { o.buttons |= PSP_CTRL_SQUARE; au->x += 1; }
            if (au->x >= 1 && player_free(g) && g->pb.t > 0.2f) { au->phase = 2; au->step_t = 0; asy_log(g, "auto: duel: wait for its swing, roll"); }
            break;
        case 2: { /* roll sideways through its next swing: start ~0.25 s before the hit frame (i-frames 0-10) */
            float hs = 0.9f;
            const CbAnim *an = cb_anim_find(&g->cbd, e->act.kind, clip_anim_id_of(e));
            if (an && an->n_hit) hs = g->cbd.hit[an->hit_first].start / 30.0f;
            if (e->ez >= 0 && e->body.t > hs - 0.2f && e->body.t < hs - 0.08f && player_free(g)) {
                float side[3] = {p->pos[0] + cosf(p->yaw) * 2, p->pos[1], p->pos[2] - sinf(p->yaw) * 2};
                au->x = side[0]; au->z = side[2];
                stick_to(g, side, 1.0f, &o);
                asy_log(g, "auto: duel: roll at %.2f s into its swing %u (hit at %.2f), dist %.2f", e->body.t,
                        clip_anim_id_of(e), hs, d);
                o.buttons |= PSP_CTRL_CROSS;
                au->phase = 7;
                au->step_t = 0;
            } else if (d > 1.3f && e->ez < 0) stick_to(g, e->pos, 0.4f, &o);
            if (au->step_t > 15.0f) { au->phase = 3; au->step_t = 0; }
            break;
        }
        case 7: { /* keep the stick sideways until the roll starts (a dodge with the stick centred = backstep) */
            float side[3] = {au->x, p->pos[1], au->z};
            if (au->step_t < 0.15f) stick_to(g, side, 1.0f, &o);
            }
            if (au->step_t > 0.2f && player_free(g)) { au->phase = 3; au->step_t = 0; asy_log(g, "auto: duel: block"); }
            break;
        case 3:   /* block: R held until a swing has been blocked (or 15 s) */
            o.buttons |= PSP_CTRL_RTRIGGER;
            if (d > 1.8f && e->ez < 0) stick_to(g, e->pos, 0.3f, &o);
            if (g->blocks > 0 && e->ez < 0) { au->phase = 4; au->step_t = 0; asy_log(g, "auto: duel: parry"); }
            if (au->step_t > 15.0f) { au->phase = 4; au->step_t = 0; }
            break;
        case 4: { /* parry: R + /\ timed to the swing (hollow 3000 hits on frames 29-33, parry window 0-7) */
            if (d > 1.1f) stick_to(g, e->pos, 0.4f, &o);
            float since = e->ez >= 0 ? e->body.t : -1;
            /* the swing's first hit frame from the combat data (TAE), parry active on its frames 0-7 */
            float hs = 0.9f;
            const CbAnim *an = cb_anim_find(&g->cbd, e->act.kind, clip_anim_id_of(e));
            if (an && an->n_hit) hs = g->cbd.hit[an->hit_first].start / 30.0f;
            if (e->ez >= 0 && since > hs - 0.2f && since < hs - 0.1f && player_free(g)) {
                o.buttons |= PSP_CTRL_RTRIGGER | PSP_CTRL_TRIANGLE;
                asy_log(g, "auto: duel: parry input at %.2f s into the hollow's swing %u (hit at %.2f), dist %.2f, hollow at (%.2f %.2f %.2f) yaw %.0f", since, clip_anim_id_of(e), hs, d, e->pos[0], e->pos[1], e->pos[2], e->yaw * 57.3f);
            }
            if (e->act.ripostable_t > 0 || (e->act.flags & CB_ST_RIPOSTABLE)) {
                au->phase = 5; au->step_t = 0;
                asy_log(g, "auto: duel: hollow open (anim %u), riposte", clip_anim_id_of(e));
            }
            if (au->step_t > 20.0f) { au->phase = 6; asy_log(g, "auto: duel: no successful parry in 20 s"); }
            break;
        }
        case 5:   /* riposte */
            if (d > 1.2f) stick_to(g, e->pos, 0.5f, &o);
            if (((int)g->frame % 4) == 0 && player_free(g)) {
                o.buttons |= PSP_CTRL_SQUARE;
                asy_log(g, "auto: duel: R1 for the riposte: dist %.2f, open %d, hollow anim %u t %.2f, player st %s",
                        d, (e->act.flags & CB_ST_RIPOSTABLE) != 0, clip_anim_id_of(e), e->body.t, asy_pst_name[p->st]);
            }
            if (p->st == P_THROW || au->step_t > 3.0f) au->phase = 6;
            break;
        default:
            if (player_free(g)) next(g, "duel done");
            break;
        }
        break;
    }
    case A_BALL: {
        if (!g->ball.on) {
            if (au->phase == 1 || au->step_t > st->arg) next(g, au->phase ? "the ball passed" : "no ball");
            break;
        }
        au->phase = 1;
        const Ball *b = &g->ball;
        float k = b->len > 0 ? b->t / b->len : 1, c[3];
        for (int i = 0; i < 3; i++) c[i] = b->p0[i] + (b->p1[i] - b->p0[i]) * (k > 1 ? 1 : k);
        float d = asy_flat_dist(c, p->pos), speed = b->len > 0 ? asy_flat_dist(b->p0, b->p1) / b->len : 5;
        if (d < b->radius + 0.4f + speed * 0.2f && player_free(g) && au->x == 0) {
            stick_to(g, c, 1.0f, &o);   /* roll into it: the roll's i-frames */
            o.buttons |= PSP_CTRL_CROSS;
            au->x = 1;
            asy_log(g, "auto: roll through the iron ball (%.1f m, %.1f m/s)", d, speed);
        } else if (au->x == 1) {
            stick_to(g, c, 1.0f, &o);
            au->x = 2;
        }
        break;
    }
    case A_HEAL:
        if (!asy_has_estus(g) || g->prg.estus <= 0 || g->pact.hp >= g->pact.hp_max * st->arg) {
            if (player_free(g) && g->prg.phase == PRG_PLAY) next(g, "healed");
            break;
        }
        if (player_free(g) && g->prg.phase == PRG_PLAY && ((int)g->frame % 10) == 0) o.buttons |= PSP_CTRL_CIRCLE;
        if (au->step_t > 20.0f) next(g, "heal timeout");
        break;
    case A_PLACE:
        for (int i = 0; i < g->n_en; i++) {
            Enemy *e = &g->en[i];
            if (e->entity != (u32)st->arg) continue;
            asy_fs(st->x, st->y, st->z, e->home);
            if (g->has_col) {
                float h;
                if (pcol_floor(&g->C, e->home[0], e->home[2], e->home[1] + 1.5f, e->home[1] - 4.0f, 0.64f, &h) >= 0) e->home[1] = h;
            }
            memcpy(e->pos, e->home, 12);
            e->yaw = e->home_yaw = 0;   /* facing -z: toward the warp point south of it in FS = +z... set below */
            e->yaw = e->home_yaw = 3.14159f;
            asy_log(g, "auto: PLACE (%s) hollow %u at (%.2f %.2f %.2f)", st->s ? st->s : "", (unsigned)e->entity,
                    e->pos[0], e->pos[1], e->pos[2]);
        }
        next(g, NULL);
        break;
    case A_LABEL:
        g_deaths_at_label = g->deaths;
        next(g, NULL);
        break;
    case A_SAVE_SNAP: {
        static u8 pb[256], eb[8192];
        int pn = prg_save_write(&g->prg, pb, sizeof pb), en = ev_save_write(&g->ev, eb, sizeof eb);
        SavePart parts[2] = {{SAVE_TAG_PROGRESS, pb, pn}, {SAVE_TAG_EVENTS, eb, en}};
        int r = save_write_file(st->s, parts, 2);
        asy_log(g, "auto: snapshot %s -> %d", st->s, r);
        next(g, NULL);
        break;
    }
    case A_LOAD_SNAP: {
        static u8 buf[16384];
        SavePart parts[SAVE_MAX_PARTS];
        int np = save_read_file(st->s, buf, sizeof buf, parts, SAVE_MAX_PARTS);
        if (np > 0) {
            const SavePart *pp = save_find(parts, np, SAVE_TAG_PROGRESS), *pe = save_find(parts, np, SAVE_TAG_EVENTS);
            if (pp) prg_save_read(&g->prg, pp->data, pp->size);
            if (pe) ev_save_read(&g->ev, pe->data, pe->size);
            ev_start(&g->ev);
            float pos[3], yaw;
            if (ev_respawn(&g->ev, pos, &yaw)) {
                memcpy(p->pos, pos, 12);
                p->yaw = yaw;
                cam_reset(&g->cam, &g->ccfg, g->row, p->pos, p->yaw);
            }
            asy_log(g, "auto: snapshot %s loaded (%d parts): player at bonfire (%.2f %.2f %.2f)", st->s, np, p->pos[0],
                    p->pos[1], p->pos[2]);
            /* skip the steps that lead to the snapshot's place: on to the next label */
            const AStep *sc = SCRIPTS[au->scenario];
            int lb = au->step + 1;
            while (sc[lb].op != A_LABEL && sc[lb].op != A_STOP) lb++;
            if (sc[lb].op == A_LABEL) au->step = lb - 1;
        } else {
            asy_log(g, "auto: snapshot %s missing (%d) -> the route to it from the start of the game", st->s, np);
        }
        next(g, NULL);
        break;
    }
    case A_RETRY:
    case A_STOP:
    default:
        if (!au->done) asy_log(g, "auto: scenario %s finished", auto_scenario_name(au->scenario));
        au->done = 1;
        break;
    }

emit:;
    float sx = o.sx, sy = o.sy, l = sqrtf(sx * sx + sy * sy);
    if (l > 1) { sx /= l; sy /= l; }
    /* never walk / roll off a ledge (except the plunge): stick world direction = the inverse of stick_to() */
    if (l > 0.05f && g_fighting && p->grounded) {
        float cy = g->lock >= 0 ? atan2f(-(g->lock_pt[0] - p->pos[0]), -(g->lock_pt[2] - p->pos[2]))
                                : (p->basis_valid ? p->basis_yaw : g->cam.yaw);
        float f = -sy, r = sx;
        float wx = -sinf(cy) * f + cosf(cy) * r, wz = -cosf(cy) * f - sinf(cy) * r;
        if (!floor_ahead(g, wx, wz, (o.buttons & PSP_CTRL_CROSS) ? ROLL_REACH : 1.2f)) {
            sx = sy = 0;
            o.buttons &= ~PSP_CTRL_CROSS;
            if (g->verbose) asy_log(g, "auto: ledge ahead, stop");
        }
    }
    out->lx = (u8)(128 + sx * 127);
    out->ly = (u8)(128 + sy * 127);
    /* X: a tap (dodge) is released the next step; held = run */
    u32 b = o.buttons;
    if ((b & PSP_CTRL_CROSS) && (au->prev_buttons & PSP_CTRL_CROSS) && st->op != A_GOTO && st->op != A_KILL &&
        st->op != A_BOSS) b &= ~PSP_CTRL_CROSS;
    /* other buttons are edges: drop if held last step so every press is a new press */
    u32 edge = PSP_CTRL_SQUARE | PSP_CTRL_TRIANGLE | PSP_CTRL_UP | PSP_CTRL_LTRIGGER | PSP_CTRL_CIRCLE | PSP_CTRL_DOWN;
    b &= ~(au->prev_buttons & edge);
    out->buttons = b;
    au->prev_buttons = b;
}
