/* Lua-driven Asylum Demon: AiWorld callbacks over the Boss body (boss_ai.h). */
#include "boss_ai.h"
#include "scene.h" /* ash_load_file */

#include <math.h>
#include <stdio.h>
#include <string.h>

#define PI 3.14159265f
#define TARGET_SELF (-1)
#define TARGET_ENE_0 0
#define POINT_INITIAL 100
#define POINT_EVENT 102

static BossAi *of(AiAgent *a) { return (BossAi *)a->user; }

/* world position of a TARGET_* / POINT_*; 0 if unknown */
static int target_pos(BossAi *ba, int t, float out[3])
{
    const Boss *b = ba->boss;
    if (t == TARGET_ENE_0 || t == 21 /* LOCALPLAYER */) { memcpy(out, b->player, 12); return 1; }
    if (t == TARGET_SELF) { memcpy(out, b->pos, 12); return 1; }
    if (t == POINT_INITIAL) { memcpy(out, ba->home, 12); return 1; }
    if (t == POINT_EVENT && ba->has_event_pt) { memcpy(out, ba->event_pt, 12); return 1; }
    return 0;
}

static float cb_dist(AiAgent *a, int t)
{
    BossAi *ba = of(a);
    const Boss *b = ba->boss;
    if (t == TARGET_ENE_0) return b->dist; /* body to body, as the C state machine [версия] */
    float p[3];
    if (!target_pos(ba, t, p)) return 9999;
    float dx = p[0] - b->pos[0], dz = p[2] - b->pos[2];
    return sqrtf(dx * dx + dz * dz);
}

static float angle_left(BossAi *ba, int t) /* radians, + = left */
{
    const Boss *b = ba->boss;
    if (t == TARGET_ENE_0) return b->angle;
    float p[3];
    if (!target_pos(ba, t, p)) return 0;
    float a = atan2f(-(p[0] - b->pos[0]), -(p[2] - b->pos[2])) - b->yaw;
    while (a > PI) a -= 2 * PI;
    while (a < -PI) a += 2 * PI;
    return a;
}

static float cb_angle(AiAgent *a, int t) { return -angle_left(of(a), t) * 180.0f / PI; }

static void cb_move(AiAgent *a, float fwd, float side, int walk, int face, float dt)
{
    BossAi *ba = of(a);
    Boss *b = ba->boss;
    float keep = b->angle;
    b->angle = angle_left(ba, a->move_target); /* boss_ext_move turns toward b->angle */
    boss_ext_move(b, fwd, side, walk, face, dt);
    if (a->move_target != TARGET_ENE_0) b->angle = keep; /* the player angle is refreshed next tick */
    ba->moved = 1;
}

static float cb_ez(AiAgent *a, int ez)
{
    BossAi *ba = of(a);
    /* CommonAttack turns toward the target before the swing [версия]: the host's Attack goal already turned (TurnTo) */
    float len = boss_request_ez(ba->boss, ez);
    if (len < 0) printf("ash-boss-ai: unknown EzState %d\n", ez);
    return len < 0 ? 0.5f : len;
}

static void cb_log(AiAgent *a, const char *s)
{
    BossAi *ba = of(a);
    snprintf(ba->boss->log, sizeof(ba->boss->log), "lua: %s", s);
    printf("ash-boss-ai: [%.2f] %s\n", a->now, s);
}

static int cb_in_region(AiAgent *a, int t, int id)
{
    float p[3];
    return target_pos(of(a), t, p) && boss_in_region(of(a)->boss, id, p);
}

static int cb_dmy_sphere(AiAgent *a, int dmy, float r, int t)
{
    BossAi *ba = of(a);
    const Boss *b = ba->boss;
    float p[3], d[3];
    if (!target_pos(ba, t, p)) return 0;
    for (u32 i = 0; i < b->hdr->n_dmy; i++) {
        if (b->dmy[i].id != dmy) continue;
        boss_dummy_world(b, (int)i, d);
        float dx = p[0] - d[0], dy = p[1] - d[1], dz = p[2] - d[2];
        return dx * dx + dy * dy + dz * dz <= r * r;
    }
    return 0;
}

static float cb_dist_y(AiAgent *a, int t)
{
    float p[3];
    return target_pos(of(a), t, p) ? p[1] - of(a)->boss->pos[1] : 0;
}

static void cb_event_target(AiAgent *a, int id)
{
    BossAi *ba = of(a);
    ba->has_event_pt = boss_region_center(ba->boss, id, ba->event_pt) == 0;
}

static int load_excel(AiAgent *a, const char *path)
{
    u8 *buf = NULL;
    int n = ash_load_file(path, &buf, NULL, 0);
    if (n <= 0) return -1;
    const char *s = (const char *)buf;
    while (s && *s) {
        int k;
        float v;
        if (sscanf(s, "excel.%d=%f", &k, &v) == 2 && k >= 0 && k < AI_MAX_EXCEL) a->excel[k] = v;
        s = strchr(s, '\n');
        if (s) s++;
    }
    return 0;
}

int boss_ai_attach(BossAi *ba, AiHost *h, Boss *b, const char *think_txt, u32 seed)
{
    memset(ba, 0, sizeof(*ba));
    ba->host = h;
    ba->boss = b;
    memcpy(ba->home, b->pos, 12);
    ba->world.get_dist = cb_dist;
    ba->world.get_angle = cb_angle;
    ba->world.move = cb_move;
    ba->world.ez_action = cb_ez;
    ba->world.log = cb_log;
    ba->world.in_region = cb_in_region;
    ba->world.dmy_sphere = cb_dmy_sphere;
    ba->world.get_dist_y = cb_dist_y;
    ba->world.set_event_target = cb_event_target;
    if (ai_agent_init(h, &ba->agent, &ba->world, "common10000_Logic", "common10000_Interupt", seed)) return -1;
    ba->agent.user = ba;
    if (think_txt && load_excel(&ba->agent, think_txt) < 0) printf("ash-boss-ai: %s missing\n", think_txt);
    ba->agent.battle_state = 1;
    ba->agent.prev_battle_state = 0;
    b->ai_mode = BOSS_AI_EXTERNAL;
    return 0;
}

void boss_ai_tick(BossAi *ba, const BossWorld *w, float dt)
{
    Boss *b = ba->boss;
    ba->bw = w;
    ba->moved = 0;
    if (b->engaged && b->hp > 0 && !w->player_dead && b->st != BS_ENTRANCE && b->st != BS_WAIT &&
        b->st != BS_INACTIVE)
        ai_agent_tick(ba->host, &ba->agent, dt);
    if (!ba->moved && b->st == BS_MOVE) boss_ext_move(b, 0, 0, 1, 0, dt); /* the goal stopped moving: idle */
    boss_tick(b, w, dt);
}

void boss_ai_interrupt(BossAi *ba, int interupt)
{
    if (ba->boss->engaged && ba->boss->hp > 0) ai_agent_interrupt(ba->host, &ba->agent, interupt);
}
