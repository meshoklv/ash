/* AI host test (PC prototype and PSP EBOOT from the same file): runs the hollow's original DSR AI bytecode against
 * a stub 2D world and logs the actions.
 *
 *   PC:  tools/ai/build_proto.sh && build/host/ai_proto ~/ash-assets/ai 250000 [seed]
 *   PSP: tools/ai/stage_psp.sh (repacks bytecode to 32-bit float, builds ash_aitest, runs PPSSPPHeadless)
 *
 * World [придумал]: player fixed at the origin, enemy on the +z axis facing it; walk 1.5 m/s, run 4 m/s, turn
 * 180 deg/s (NpcParam.TurnVelocity of 250022); every EzState action lasts 1.2 s, steps 0.6 s with a 2 m shift.
 * Scenario A: for each start distance, think once and log the first chosen action.
 * Scenario B: 60 s of continuous fight with the player standing still, plus an INTERUPT_Damaged every 7 s.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aihost.h"

#ifdef __PSP__
#include <pspkernel.h>
#include <psppower.h>
#include <psprtc.h>
PSP_MODULE_INFO("Ash AI test", 0, 0, 1);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);
PSP_HEAP_SIZE_KB(-1024);
static uint64_t now_us(void) {
    u64 t;
    sceRtcGetCurrentTick(&t); /* 1 MHz */
    return t;
}
#else
#include <time.h>
static uint64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + ts.tv_nsec / 1000;
}
#endif
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

typedef struct {
    float x, z, yaw; /* yaw: radians, 0 = facing -z (towards the player when on +z) */
    int verbose;
    int first_attack;
    float first_attack_dist;
    int first_step;
} Body;

static Body body;

static float dist(AiAgent *a, int t) {
    (void)a;
    if (t == -1) return 0;
    return sqrtf(body.x * body.x + body.z * body.z);
}
static float angle_to_player(AiAgent *a, int t) {
    (void)a; (void)t;
    float want = atan2f(-body.x, body.z); /* forward = (sin yaw, -cos yaw) */
    float d = want - body.yaw;
    while (d > (float)M_PI) d -= 2 * (float)M_PI;
    while (d < -(float)M_PI) d += 2 * (float)M_PI;
    return d * 57.29578f;
}
static void move(AiAgent *a, float fwd, float side, int walk, int face, float dt) {
    if (face) {
        float d = angle_to_player(a, 0) / 57.29578f, lim = 3.14159f * dt; /* 180 deg/s */
        body.yaw += d > lim ? lim : d < -lim ? -lim : d;
    }
    float sp = (walk ? 1.5f : 4.0f) * dt;
    float fx = sinf(body.yaw), fz = cosf(body.yaw) * -1.f; /* forward */
    float rx = -fz, rz = fx;                                  /* right */
    body.x += (fx * fwd + rx * side) * sp;
    body.z += (fz * fwd + rz * side) * sp;
}
static float ez_action(AiAgent *a, int ez) {
    (void)a;
    if (ez >= 700 && ez <= 703) {
        float fx = sinf(body.yaw), fz = -cosf(body.yaw);
        float k = ez == 701 ? -2.f : ez == 700 ? 2.f : 0;
        body.x += fx * k; body.z += fz * k;
        if (!body.first_step) body.first_step = ez;
        return 0.6f;
    }
    if (!body.first_attack) {
        body.first_attack = ez;
        body.first_attack_dist = dist(a, 0);
    }
    return 1.2f;
}
static void logline(AiAgent *a, const char *s) {
    if (body.verbose) printf("[%6.2f] d=%5.2f  %s\n", a->now, dist(a, 0), s);
}

static int load_excel(AiAgent *a, const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char ln[256];
    while (fgets(ln, sizeof ln, f)) {
        int n;
        double v;
        if (sscanf(ln, "excel.%d=%lf", &n, &v) == 2 && n >= 0 && n < AI_MAX_EXCEL) a->excel[n] = v;
    }
    fclose(f);
    return 0;
}

static const char *COMMON[] = {"ai_define", "goal_list", "goal_list_dlc", "logic_list", "event_list",
                               "common_func", "common_func2", "common_battle_func", "common_logic_func", "top_goal",
                               "010000_logic", "approach_target", "attack", "attack_tunable_spin", "combo_attack",
                               "combo_final", "combo_repeat", "common_attack", "gaurd_break_attack", NULL};

static int run(int argc, char **argv);
int main(int argc, char **argv) {
#ifdef __PSP__
    scePowerSetClockFrequency(333, 333, 166);
    int rc = run(1, argv);
    printf("ash-aitest: exit %d\n", rc);
    sceKernelExitGame();
    return 0;
#else
    return run(argc, argv);
#endif
}

static int run(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : "ai";
    int goal = argc > 2 ? atoi(argv[2]) : 250000;
    unsigned seed = argc > 3 ? (unsigned)strtoul(argv[3], 0, 0) : 1;
    /* NpcThinkParam export (tools/ai/think_export.py): the hollow's by default, think_<goal>.txt otherwise */
    char thinkbuf[64];
    snprintf(thinkbuf, sizeof thinkbuf, goal == 250000 ? "think_250022.txt" : "think_%d.txt", goal);
    const char *think = argc > 4 ? argv[4] : thinkbuf;
    char p[512];
    AiHost *h = ai_host_new();
    ai_host_set_clock(h, now_us);
    /* aiCommon in binder order (load_order.txt from tools/ai/extract_ai.py), without other enemies' battle/logic
       scripts except 010000_logic; fallback: the fixed list the hollow needs */
    snprintf(p, sizeof p, "%s/aiCommon/load_order.txt", dir);
    FILE *lo = fopen(p, "r");
    int nloaded = 0;
    if (lo) {
        char nm[128];
        while (fgets(nm, sizeof nm, lo)) {
            nm[strcspn(nm, "\r\n")] = 0;
            size_t n = strlen(nm);
            int enemy = (n > 7 && !strcmp(nm + n - 7, "_battle")) || (n > 6 && !strcmp(nm + n - 6, "_logic"));
            if (!*nm || (enemy && strcmp(nm, "010000_logic"))) continue;
            snprintf(p, sizeof p, "%s/aiCommon/%s.luac", dir, nm);
            if (ai_host_load(h, p)) return 1;
            nloaded++;
        }
        fclose(lo);
    } else {
        for (int i = 0; COMMON[i]; i++) {
            snprintf(p, sizeof p, "%s/aiCommon/%s.luac", dir, COMMON[i]);
            if (ai_host_load(h, p)) return 1;
            nloaded++;
        }
    }
    printf("loaded %d aiCommon chunks (%s)\n", nloaded, lo ? "load_order.txt" : "built-in list");
    snprintf(p, sizeof p, "%s/m18_01_00_00/%06d_battle.luac", dir, goal);
    if (ai_host_load(h, p)) return 1;

    static const AiWorld w = {.get_dist = dist, .get_angle = angle_to_player, .move = move, .ez_action = ez_action,
                              .log = logline};
    AiAgent a;

    /* ---- A: first action per start distance, N seeds each */
    static const float D[] = {0.8f, 1.5f, 2.0f, 3.0f, 4.5f, 6.0f, 7.0f, 9.0f, 12.0f};
    printf("== A: first action after the first think, 200 seeds per distance (goal %d)\n", goal);
    printf("start_d  | first EzState chosen (count)                  | first step | mean dist at attack\n");
    for (unsigned k = 0; k < sizeof D / sizeof *D; k++) {
        int hist[64] = {0}, steps = 0, none = 0;
        double dsum = 0;
        int nd = 0;
        for (unsigned s = 0; s < 200; s++) {
            memset(&a, 0, sizeof a);
            memset(&body, 0, sizeof body);
            body.z = D[k];
            if (ai_agent_init(h, &a, &w, "common10000_Logic", "common10000_Interupt", seed * 7919u + s * 104729u + 1))
                return 1;
            load_excel(&a, (snprintf(p, sizeof p, "%s/%s", dir, think), p));
            a.battle_state = 1;
            a.prev_battle_state = 1;
            for (int t = 0; t < 30 * 15 && !body.first_attack; t++) ai_agent_tick(h, &a, 1.f / 30);
            if (body.first_attack >= 3000 && body.first_attack < 3064) {
                hist[body.first_attack - 3000]++;
                dsum += body.first_attack_dist;
                nd++;
            } else
                none++;
            if (body.first_step) steps++;
            ai_agent_free(h, &a);
        }
        printf("%6.1f m | ", D[k]);
        int n = 0;
        for (int i = 0; i < 64; i++)
            if (hist[i]) n += printf("%d:%d ", 3000 + i, hist[i]);
        if (none) n += printf("none:%d", none);
        printf("%*s| %3d        | %.2f m\n", n < 46 ? 46 - n : 0, "", steps, nd ? dsum / nd : 0);
    }

    /* ---- B: verbose 20 s fight */
    printf("\n== B: 20 s fight log, start 9 m, seed %u, INTERUPT_Damaged(2) at t=7 s and t=14 s\n", seed);
    memset(&a, 0, sizeof a);
    memset(&body, 0, sizeof body);
    body.z = 9;
    body.verbose = 1;
    ai_agent_init(h, &a, &w, "common10000_Logic", "common10000_Interupt", seed);
    load_excel(&a, (snprintf(p, sizeof p, "%s/%s", dir, think), p));
    a.battle_state = 1;
    a.prev_battle_state = 0;
    for (int t = 0; t < 30 * 20; t++) {
        if (t == 30 * 7 || t == 30 * 14) {
            printf("[%6.2f] d=%5.2f  !! raise INTERUPT_Damaged\n", a.now, dist(&a, 0));
            ai_agent_interrupt(h, &a, 2);
        }
        body.first_attack = 0;
        ai_agent_tick(h, &a, 1.f / 30);
    }
    const AiStats *st = ai_host_stats(h);
    printf("\n== stats (all runs; emulator time is not PSP time): lua calls %lu, time in Lua %.1f ms, worst call %lu us, "
           "goal pool peak %d, Lua heap %d KB\n", (unsigned long)st->lua_calls, st->lua_us / 1000.0, (unsigned long)st->lua_us_max,
           st->goals_peak, st->lua_kb);
    const char **un;
    const int *uc;
    int nu = ai_host_unknown(h, &un, &uc);
    printf("\n== unimplemented methods called: %d\n", nu);
    for (int i = 0; i < nu; i++) printf("  %s x%d\n", un[i], uc[i]);
    ai_host_free(h);
    return 0;
}
