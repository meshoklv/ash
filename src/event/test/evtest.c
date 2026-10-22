/*
 * Walkthrough test of the event script of m18_01 (Northern Undead Asylum) without the game: a fake host (positions,
 * HP, animations as fixed durations) and a scripted player who walks between waypoints (event blockers applied:
 * closed doors / gate / fog stop him), presses the action button, kills the demon, dies once, and reaches the
 * crow. Every step checks the script's reaction (flags, items, prompts, commands). Same source runs natively
 * (scripts/test-events.sh) and on the PSP / PPSSPPHeadless (EBOOT ash_evtest). Output: log + "TEST:{...}".
 *
 *     evtest <file.ashevt> [-v]
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../event.h"
#define EVOPS_NAMES
#include "../evops.h"

#ifdef __PSP__
#include <pspkernel.h>
PSP_MODULE_INFO("ash_evtest", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER);
PSP_HEAP_SIZE_KB(4096);
#endif

#define DT (1.0f / 30.0f)

/* ------------------------------------------------------------------ fake world */

typedef struct { uint32_t id; float pos[3], yaw, hp; int dead, enabled, ai; float dead_timer; } FChr;
static FChr g_chr[48];
static int g_nchr;
static Ev g_ev;
static float g_pos[3], g_yaw;           /* player, PSP space */
static const char *g_floor;             /* collision part under the player */
static float g_through;                 /* seconds left of the fog walk (anim 7410) */
static int g_verbose, g_fail, g_checks, g_cmds[EV_CMD_COUNT];
static char g_last_dialog[160], g_last_item[160], g_banner[160];
static int g_end_demo, g_reloads, g_saves, g_bombs, g_balls, g_boss_bar, g_killboss;
static float g_t;

static FChr *chr(uint32_t id)
{
    for (int i = 0; i < g_nchr; i++) if (g_chr[i].id == id) return &g_chr[i];
    return 0;
}

static float q_query(void *u, int what, uint32_t id, uint32_t arg)
{
    (void)u; (void)arg;
    FChr *c = chr(id);
    if (what == EVQ_HP_RATIO) return c ? c->hp : 1.0f;
    if (what == EVQ_DEAD) return c ? (float)c->dead : 0.0f;
    return 0.0f;
}

static int q_pos(void *u, uint32_t id, float p[3], float *yaw)
{
    (void)u;
    FChr *c = chr(id);
    if (!c) return 0;
    memcpy(p, c->pos, 12);
    if (yaw) *yaw = c->yaw;
    return 1;
}

static const char *cmd_names[EV_CMD_COUNT] = {
    "LOG", "CHR_SET", "KILL", "CHR_ANIM", "MOVE", "FACE", "STANDBY", "SPEFFECT", "NEST", "AI_CMD", "BOSS_BAR",
    "BOSS_START", "KILL_BOSS", "OBJ_ANIM", "OBJ_STATE", "VFX", "MAPCOL", "DIALOG", "CUTSCENE", "SOUND", "SFX",
    "ITEM", "OBJACT", "LADDER", "BONFIRE_LIT", "REST", "RELOAD", "SAVE", "BALL", "BOMB", "BANNER", "END_DEMO"};

static float q_cmd(void *u, const EvCmd *c)
{
    (void)u;
    float ret = 0;
    if (c->op >= 0 && c->op < EV_CMD_COUNT) g_cmds[c->op]++;
    switch (c->op) {
    case EV_CMD_MOVE:
        if (c->id == EV_PLAYER) { memcpy(g_pos, c->pos, 12); g_yaw = c->yaw; }
        else if (chr(c->id)) { memcpy(chr(c->id)->pos, c->pos, 12); chr(c->id)->yaw = c->yaw; }
        break;
    case EV_CMD_FACE:
        if (c->id == EV_PLAYER) g_yaw = atan2f(-(c->pos[0] - g_pos[0]), -(c->pos[2] - g_pos[2]));
        break;
    case EV_CMD_CHR_ANIM:
        if (c->id == EV_PLAYER && c->a == 7410) { g_through = 1.5f; ret = 1.5f; }   /* walk through the fog */
        else ret = (c->a >= 9000) ? 3.0f : 1.0f;                                   /* demon 9060: drop down */
        break;
    case EV_CMD_CHR_SET: {
        FChr *ch = chr(c->id);
        if (ch && c->a == EV_CHR_ENABLE) ch->enabled = c->b;
        if (ch && c->a == EV_CHR_AI) ch->ai = c->b;
        break;
    }
    case EV_CMD_KILL: if (chr(c->id)) { chr(c->id)->hp = 0; chr(c->id)->dead = 1; } break;
    case EV_CMD_OBJ_ANIM: ret = c->f[0] < 0 ? 0 : 1.0f; break;
    case EV_CMD_CUTSCENE: ret = 2.0f; break;
    case EV_CMD_LADDER: memcpy(g_pos, c->pos, 12); g_yaw = c->yaw; ret = 1.0f; break;
    case EV_CMD_REST: ret = 2.0f; break;
    case EV_CMD_RELOAD: g_reloads++; break;
    case EV_CMD_SAVE: g_saves++; break;
    case EV_CMD_BALL: g_balls++; break;
    case EV_CMD_BOMB: g_bombs++; break;
    case EV_CMD_BOSS_BAR: g_boss_bar = c->a; break;
    case EV_CMD_KILL_BOSS: g_killboss++; break;
    case EV_CMD_DIALOG: snprintf(g_last_dialog, sizeof g_last_dialog, "%s", c->text ? c->text : "?"); break;
    case EV_CMD_OBJACT: if (!c->b) snprintf(g_last_dialog, sizeof g_last_dialog, "%s", c->text ? c->text : "?"); break;
    case EV_CMD_ITEM: snprintf(g_last_item, sizeof g_last_item, "%s x%d", c->text ? c->text : "?", (int)c->b); break;
    case EV_CMD_END_DEMO: g_end_demo = 1; snprintf(g_banner, sizeof g_banner, "%s", c->text ? c->text : "?"); break;
    }
    if (g_verbose || c->op == EV_CMD_ITEM || c->op == EV_CMD_DIALOG || c->op == EV_CMD_END_DEMO ||
        c->op == EV_CMD_KILL_BOSS || c->op == EV_CMD_RELOAD || c->op == EV_CMD_CUTSCENE || c->op == EV_CMD_BOSS_BAR ||
        c->op == EV_CMD_OBJACT || c->op == EV_CMD_LADDER || c->op == EV_CMD_BALL || c->op == EV_CMD_BOMB ||
        c->op == EV_CMD_REST || c->op == EV_CMD_BONFIRE_LIT || (c->op == EV_CMD_OBJ_ANIM && c->f[0] >= 0) ||
        (c->op == EV_CMD_CHR_ANIM && c->a >= 7000) || c->op == EV_CMD_LOG)
        printf("  t=%6.2f %-11s id=%u a=%d b=%d%s%s\n", g_t, cmd_names[c->op], (unsigned)c->id, (int)c->a, (int)c->b,
               c->text ? " " : "", c->text ? c->text : "");
    return ret;
}

/* ------------------------------------------------------------------ driver */

static void tick(int interact)
{
    EvInput in;
    memcpy(in.pos, g_pos, 12); in.yaw = g_yaw; in.interact = interact; in.floor_part = g_floor;
    ev_tick(&g_ev, DT, &in);
    g_t += DT;
    if (g_through > 0) {                       /* fog walk: 2 m forward over 1.5 s, fog not blocking */
        g_through -= DT;
        g_pos[0] += -sinf(g_yaw) * (2.0f / 1.5f) * DT;
        g_pos[2] += -cosf(g_yaw) * (2.0f / 1.5f) * DT;
    } else {
        ev_push_out(&g_ev, g_pos, 0.4f, 0);
    }
    for (int i = 0; i < g_nchr; i++)
        if (g_chr[i].hp <= 0 && !g_chr[i].dead) {
            g_chr[i].dead_timer += DT;
            if (g_chr[i].dead_timer > 1.0f) g_chr[i].dead = 1;
        }
}

static void wait_s(float s) { for (float t = 0; t < s; t += DT) tick(0); }

static void face(const float p[3]) { g_yaw = atan2f(-(p[0] - g_pos[0]), -(p[2] - g_pos[2])); }

/* FromSoft coordinates in the script (as MSB / docs), mirrored here */
static void fs(float x, float y, float z, float out[3]) { out[0] = -x; out[1] = y; out[2] = z; }

/* walk in a straight line at 4 m/s; returns the distance left (blocked if > 0.3) */
static float walk(float x, float y, float z)
{
    float t[3]; fs(x, y, z, t);
    for (int n = 0; n < 30 * 40; n++) {
        float dx = t[0] - g_pos[0], dy = t[1] - g_pos[1], dz = t[2] - g_pos[2];
        float d = sqrtf(dx * dx + dz * dz);
        if (d < 0.05f) break;
        float step = 4.0f * DT;
        if (step > d) step = d;
        face(t);
        float before[3]; memcpy(before, g_pos, 12);
        g_pos[0] += dx / d * step; g_pos[2] += dz / d * step;
        g_pos[1] += dy * (step / (d + 1e-6f));
        tick(0);
        float moved = sqrtf((g_pos[0] - before[0]) * (g_pos[0] - before[0]) + (g_pos[2] - before[2]) * (g_pos[2] - before[2]));
        if (moved < 0.01f) break;               /* blocked */
    }
    float dx = t[0] - g_pos[0], dz = t[2] - g_pos[2];
    return sqrtf(dx * dx + dz * dz);
}

static void check(int ok, const char *what)
{
    g_checks++;
    if (!ok) g_fail++;
    printf("%s %s\n", ok ? "  ok  " : "  FAIL", what);
}

/* press the action button when `want` is the prompt (looks for it up to 1 s) */
static int press(const char *want)
{
    for (int i = 0; i < 30; i++) {
        tick(0);
        const char *p = ev_prompt(&g_ev);
        if (p && (!want || strstr(p, want))) {
            printf("  press [%s]\n", p);
            tick(1);
            return 1;
        }
    }
    const char *p = ev_prompt(&g_ev);
    printf("  no prompt '%s' (have '%s')\n", want ? want : "*", p ? p : "-");
    return 0;
}

static void look_at_fs(float x, float y, float z) { float t[3]; fs(x, y, z, t); face(t); }

static int has(uint32_t cat, uint32_t id) { return ev_has_item(&g_ev, (cat << 28) | id); }

static void add_chr(uint32_t id)
{
    const EvfEnt *e = ev_ent(&g_ev, id);
    FChr *c = &g_chr[g_nchr++];
    memset(c, 0, sizeof *c);
    c->id = id; c->hp = 1.0f; c->enabled = 1;
    if (e) { memcpy(c->pos, e->pos, 12); c->yaw = e->yaw; }
}

static uint8_t *read_file(const char *path, int *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = (uint8_t *)malloc((size_t)n + 16);
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); b = 0; }
    fclose(f);
    *size = (int)n;
    return b;
}

static int run(const char *path)
{
    int size = 0;
    uint8_t *file = read_file(path, &size);
    if (!file) { printf("cannot read %s\n", path); return 2; }
    int ms = ev_mem_size(file, size);
    void *mem = calloc(1, (size_t)(ms > 0 ? ms : 16));
    EvHost host = { 0, q_query, q_pos, q_cmd };
    int rc = ev_load(&g_ev, file, size, mem, &host);
    printf("ev_load=%d size=%d mem=%d inst=%u ent=%u treas=%u lots=%u str=%u objact=%u\n", rc, size, ms,
           (unsigned)g_ev.n_inst, (unsigned)g_ev.n_ent, (unsigned)g_ev.n_treas, (unsigned)g_ev.n_lot,
           (unsigned)g_ev.n_str, (unsigned)g_ev.n_objact);
    if (rc) return 2;
    static const uint32_t chrs[] = {1810800, 1810810, 1810101, 1810102, 1810103, 1810104, 1810106, 1810107,
                                    1810108, 1810110, 1810111, 1810112, 1810113, 1810200, 1810213};
    for (unsigned i = 0; i < sizeof chrs / sizeof chrs[0]; i++) add_chr(chrs[i]);

    printf("== 1. wake up in the cell\n");
    ev_start(&g_ev);
    wait_s(3.0f);
    check(ev_flag(&g_ev, 11810002), "opening played once (flag 11810002), player warped to the post-cutscene start");
    float sp[3]; fs(-15.45f, 184.7f, -46.8f, sp);
    check(fabsf(g_pos[0] - sp[0]) < 0.1f && fabsf(g_pos[2] - sp[2]) < 0.1f, "player at region 1812011");
    check(!chr(1810200)->enabled && !chr(1810213)->enabled, "Black Knights (second visit) off on the first visit");

    printf("== 2. key on the floor, cell door\n");
    walk(-14.6f, 184.75f, -44.0f);
    check(walk(-14.6f, 184.75f, -41.5f) > 0.3f, "closed cell door blocks the way out");
    walk(-13.6f, 184.75f, -44.6f);
    look_at_fs(-13.6f, 185.0f, -45.24f);
    check(press("Поднять") && has(4, 2010), "picked up Dungeon Cell Key (goods 2010)");
    walk(-13.8f, 184.75f, -43.3f);
    look_at_fs(-13.8f, 185.2f, -42.47f);
    check(press("Открыть"), "cell door offers «Открыть»");
    wait_s(2.5f);
    check(ev_flag(&g_ev, 11810103), "door 1811103 used (objact flag 11810103)");
    check(strstr(g_last_dialog, "Ключ от темницы") != 0, "dialog «Использовано: Ключ от темницы…»");
    check(walk(-13.8f, 184.75f, -41.0f) < 0.3f, "door open: walked out of the cell");

    printf("== 3. corridor, ladder up to the courtyard\n");
    walk(-14.0f, 185.0f, 0.0f);
    walk(-0.9f, 189.49f, 22.6f);
    check(press("Подняться"), "ladder 1811140 offers «Подняться»");
    check(g_pos[1] > 195.5f, "ladder: player at the top (y > 195.5)");

    printf("== 4. courtyard bonfire, big door\n");
    walk(3.1f, 195.9f, 6.2f);
    check(press("Разжечь") && ev_flag(&g_ev, 11810992), "bonfire 1 lit (flag 11810992)");
    walk(3.19f, 198.08f, -2.6f);
    look_at_fs(3.19f, 198.5f, -4.03f);
    check(press("Открыть"), "big door 1811111 «Открыть»");
    wait_s(1.0f);
    check(ev_flag(&g_ev, 11810112), "big door opened (flag 11810112)");

    printf("== 5. hall: the demon drops, run through the gate\n");
    walk(3.1f, 198.0f, -14.0f);
    wait_s(4.0f);
    check(ev_flag(&g_ev, 11810314) && ev_flag(&g_ev, 11810310), "first encounter played (11810314, 11810310)");
    check(chr(1810800)->ai == 1, "demon AI on (11815392)");
    check(g_boss_bar == 1, "boss HP bar on (11815394)");
    EvObjInfo oi; int gate_open = 0, door_open = 1, fog_on = 0;
    for (int i = 0; i < (int)g_ev.n_ent; i++) if (ev_obj_get(&g_ev, i, &oi)) {
        if (oi.id == 1811115) gate_open = oi.open;
        if (oi.id == 1811111) door_open = oi.open;
        if (oi.id == 1811990) fog_on = oi.enabled;
    }
    check(gate_open && !door_open && fog_on, "gate open, big door shut behind, balcony fog lit");
    walk(14.0f, 198.4f, -25.5f);
    walk(16.5f, 198.4f, -25.5f);                  /* through the open gate (x 15.37) */
    g_floor = "h0006B1";                           /* corridor floor behind it = collision 1813120 */
    walk(18.0f, 198.4f, -25.5f);
    wait_s(1.5f);
    check(ev_flag(&g_ev, 11810312) && g_ev.respawn == 1812961, "escaped: gate closing, respawn = bonfire 2 (1812961)");
    check(g_saves > 0, "autosave requested");
    wait_s(1.0f);
    check(g_boss_bar == 0, "boss bar off outside the hall (11815395)");
    if (getenv("EVDBG")) for (uint32_t i = 0; i < g_ev.n_inst; i++)
        if (g_ev.inst[i].event_id / 10 == 1181539 || g_ev.inst[i].event_id == 11810311)
            printf("    dbg inst %u ev %u st %d pc %d/%u f390=%d pos=%.2f %.2f %.2f\n", (unsigned)i, (unsigned)g_ev.inst[i].event_id,
                   g_ev.run[i].status, g_ev.run[i].pc, (unsigned)g_ev.inst[i].n, ev_flag(&g_ev, 11815390), g_pos[0], g_pos[1], g_pos[2]);
    check(walk(13.0f, 198.4f, -25.5f) > 0.3f, "gate shut: no way back into the hall");

    printf("== 6. death and reload at bonfire 2\n");
    g_floor = 0;
    ev_player_died(&g_ev);
    tick(0);
    float rp[3], ry;
    check(g_reloads == 1 && ev_respawn(&g_ev, rp, &ry) && fabsf(rp[0] + 33.9f) < 0.1f, "reload, respawn at bonfire 2");
    memcpy(g_pos, rp, 12); g_yaw = ry;
    wait_s(0.5f);
    check(ev_flag(&g_ev, 11810310) && ev_flag(&g_ev, 11810312), "first-encounter flags survive the death");
    check(!ev_flag(&g_ev, 11815393) && !ev_flag(&g_ev, 11815392), "volatile boss flags (1181539x) cleared");

    printf("== 7. bonfire 2, starting weapon + shield (class Warrior)\n");
    walk(33.34f, 193.15f, -25.6f);
    check(press("Разжечь") && ev_flag(&g_ev, 11810984), "bonfire 2 lit (flag 11810984)");
    walk(37.61f, 192.66f, -11.1f);
    check(press("Поднять") && has(0, 1450000), "Heater Shield (1450000) from 1811602");
    walk(38.4f, 195.42f, 16.2f);
    check(press("Поднять") && has(0, 201000), "Longsword (201000) from 1811601");
    check(!has(0, 202000), "no Knight items (Broadsword) — class fixed at cook time");

    printf("== 8. fog CP1, Estus, iron ball, firebombs\n");
    walk(6.37f, 201.78f, 19.63f);
    check(press("белый свет"), "fog 1811700 «Пройти через белый свет»");
    wait_s(2.5f);
    check(ev_flag(&g_ev, 11810090) && g_pos[0] > -5.6f, "passed CP1 fog (flag 11810090, player beyond x=5.4)");
    walk(-4.6f, 201.96f, 22.0f);
    check(press("Поднять") && has(4, 200) && has(4, 2012), "Estus Flask + East Wing key where Oscar lies");
    walk(-8.0f, 203.3f, 7.0f);
    wait_s(3.0f);
    check(g_balls == 1, "iron ball released (11810211)");
    int wall_broken = 0;
    for (int i = 0; i < (int)g_ev.n_ent; i++) if (ev_obj_get(&g_ev, i, &oi) && oi.id == 1811210) wall_broken = oi.destroyed || !oi.enabled;
    check(wall_broken, "wall 1811210 broken by the ball (11810200)");
    check(g_bombs >= 3, "firebomb trap: 3 bombs thrown at the player on the stairs");
    walk(-8.0f, 208.4f, -5.0f);
    walk(-3.0f, 208.4f, -36.0f);

    printf("== 9. balcony fog, plunge, kill the Asylum Demon\n");
    walk(3.2f, 209.0f, -35.3f);
    look_at_fs(3.2f, 209.0f, -27.6f);
    check(press("белый свет"), "boss fog 1811990 «Пройти через белый свет»");
    wait_s(2.0f);
    float dp[3]; fs(3.0f, 197.98f, -27.0f, dp);
    check(fabsf(chr(1810800)->pos[2] - dp[2]) < 0.1f, "demon moved under the balcony (1812301) for the plunge");
    check(ev_flag(&g_ev, 11815393) && chr(1810800)->ai == 1, "boss fight on, AI on");
    fs(3.0f, 198.0f, -26.0f, g_pos);              /* plunge: land next to the demon */
    wait_s(0.5f);
    check(g_boss_bar == 1, "boss bar on in the arena");
    check(walk(18.0f, 198.4f, -25.5f) > 0.3f, "gate 1811115 shut: no running away from the fight");
    walk(3.0f, 198.0f, -26.0f);
    chr(1810800)->hp = 0;                          /* the fight itself is the combat code's job */
    wait_s(2.5f);
    check(ev_flag(&g_ev, 16) && g_killboss == 1, "demon dead: flag 16, KillBoss");
    check(has(4, 2011), "Big Pilot Key (goods 2011) dropped (lot 2660)");
    check(!has(0, 852000), "no Demon's Great Hammer (only for a first-encounter kill)");
    check(g_boss_bar == 0, "boss bar off");

    printf("== 10. back door with the Big Pilot Key, the crow\n");
    walk(3.19f, 198.08f, -32.0f);
    look_at_fs(3.19f, 198.5f, -33.42f);
    check(press("Открыть"), "small church door 1811110 «Открыть»");
    wait_s(1.0f);
    check(strstr(g_last_dialog, "Большой ключ") != 0, "dialog «Использовано: Большой ключ паломника»");
    check(walk(3.19f, 198.08f, -36.0f) < 0.3f, "door open: out to the courtyard");
    walk(2.0f, 198.0f, -60.0f);
    walk(3.5f, 210.0f, -96.0f);
    wait_s(1.0f);
    check(g_end_demo && strstr(g_banner, "Конец демо") != 0, "crow at the cliff: «Конец демо»");

    printf("== 11. save / load round trip\n");
    static uint8_t save[8192];
    int n = ev_save_write(&g_ev, save, sizeof save);
    Ev *e2 = (Ev *)calloc(1, sizeof(Ev));
    void *mem2 = calloc(1, (size_t)ms);
    ev_load(e2, file, size, mem2, &host);
    int rr = ev_save_read(e2, save, n);
    int same = rr == 0 && e2->respawn == g_ev.respawn && ev_flag(e2, 16) && ev_flag(e2, 11810312) &&
               !ev_flag(e2, 11815393) && ev_has_item(e2, (4u << 28) | 2011);
    check(same, "save blob restores flags, respawn and keys");
    printf("save bytes=%d flags=%d items=%d\n", n, g_ev.n_flags, g_ev.n_items);

    printf("== 12. new game: die in the first encounter -> the encounter resets\n");
    {
        int reload0 = g_reloads;
        memset(&g_ev, 0, sizeof g_ev);
        memset(mem, 0, (size_t)ms);
        ev_load(&g_ev, file, size, mem, &host);
        g_nchr = 0;
        for (unsigned i = 0; i < sizeof chrs / sizeof chrs[0]; i++) add_chr(chrs[i]);
        g_end_demo = 0; g_floor = 0;
        ev_start(&g_ev);
        wait_s(3.0f);
        fs(3.1f, 198.0f, -14.0f, g_pos);          /* teleport into the hall */
        wait_s(4.5f);
        check(ev_flag(&g_ev, 11810314) && ev_flag(&g_ev, 11810310), "encounter played");
        ev_player_died(&g_ev);
        fs(-13.75f, 184.75f, -44.5f, g_pos);      /* respawn point 1812900 = start of the game */
        tick(0); wait_s(0.5f);
        check(g_reloads == reload0 + 1, "reload");
        check(!ev_flag(&g_ev, 11810310) && !ev_flag(&g_ev, 11810314), "encounter flags reset (no escape yet)");
        float ap[3]; fs(3.59f, 226.15f, -36.62f, ap);
        check(fabsf(chr(1810800)->pos[1] - ap[1]) < 0.1f, "demon back in the air above the hall (1812305)");
        fs(3.1f, 198.0f, -14.0f, g_pos);
        wait_s(4.5f);
        check(ev_flag(&g_ev, 11810310), "encounter plays again");
    }
    free(mem2); free(e2); free(mem); free(file);
    printf("TEST:{\"suite\":\"events_m18_01\",\"checks\":%d,\"fail\":%d,\"t\":%.1f,\"reloads\":%d,\"saves\":%d,"
           "\"bombs\":%d,\"flags\":%d,\"items\":%d,\"save_bytes\":%d,\"mem\":%d}\n",
           g_checks, g_fail, g_t, g_reloads, g_saves, g_bombs, g_ev.n_flags, g_ev.n_items, n, ms);
    return g_fail ? 1 : 0;
}

int main(int argc, char **argv)
{
    const char *path = "m18_01.ashevt";
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-v")) g_verbose = 1;
        else path = argv[i];
    }
#ifdef __PSP__
    static const char *cands[] = {"m18_01.ashevt", "host0:/m18_01.ashevt", "ms0:/ash/m18_01.ashevt"};
    FILE *f = 0;
    for (unsigned i = 0; i < 3 && !f; i++) if ((f = fopen(cands[i], "rb"))) { path = cands[i]; fclose(f); }
#endif
    int rc = run(path);
#ifdef __PSP__
    sceKernelExitGame();
#endif
    return rc;
}
