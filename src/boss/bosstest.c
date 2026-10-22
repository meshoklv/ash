/*
 * Ash boss test: the Asylum Demon (c2232) in its arena (upper hall of m18_01), driven by src/boss (state machine
 * from 223200_battle), against a scripted stand-in player (the Elite Knight, idle) - no pad needed, for
 * PPSSPPHeadless. Shows: the drop from the ceiling (a00_9060), the fight (approach, sweeps, smashes, foot
 * stomps, side steps), the balcony phase (3020/3021 + the plunge attack from above), the death; HP bar with
 * the Russian name, attack hit volumes as markers. Player damage numbers here are placeholders [придумал]:
 * the real ones come from the combat module.
 *
 * Files next to the EBOOT: scene.ashm (arena_nochars), knight.ashc/.ashanim, boss.ashc/.ashanim/.ashboss
 * (tools/cook_boss.py), boss_name.ashtx (tools/cook_boss.py), demo.txt "<dump_every> <frames>".
 * Stdout "ash-boss: ..." / "ash-bosstest: ...". Emulated timings are not hardware timings.
 */
#include <pspdebug.h>
#include <pspdisplay.h>
#include <pspgu.h>
#include <pspgum.h>
#include <pspiofilemgr.h>
#include <pspkernel.h>
#include <psprtc.h>
#include <malloc.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "anim.h"
#include "boss.h"
#include "chr.h"
#include "scene.h"
#include "pcol.h"

/* the demon's body against the arena's Havok collision (arena.pcol), when staged [версия: capsule by NpcParam
 * HitRadius 2.5 minus 0.5 so it does not snag on pillars' bases] */
static Pcol g_col;
static float g_boss_vy;
static void boss_collide(void *ctx, float pos[3], float dx, float dz)
{
    static const PcolCapsule cap = {2.0f, 7.5f, 0.6f, 0.64f, 0.6f, 9.8f};
    int tri;
    pcol_move((Pcol *)ctx, &cap, pos, dx, dz, &g_boss_vy, 1.0f / 30.0f, &tri);
}
#ifdef BOSS_LUA
#include "boss_ai.h"
/* Lua mode: ai/lua.txt staged next to the EBOOT (scripts/shoot-boss.sh ... lua) -> the original 223200_battle on
 * the src/ai host drives the demon instead of the C state machine */
static AiHost *g_ai_host;
static BossAi g_ai;
static int g_lua;
static int lua_setup(Boss *b)
{
    u8 *buf = NULL;
    int n = ash_load_file("ai/aiCommon/load_order.txt", &buf, NULL, 0);
    if (n <= 0 || ash_load_file("ai/lua.txt", &(u8 *){NULL}, NULL, 0) <= 0) return 0;
    g_ai_host = ai_host_new();
    int loaded = 0;
    char *s = (char *)buf, path[128];
    while (s && *s) {
        char *e = strpbrk(s, "\r\n");
        int len = e ? (int)(e - s) : (int)strlen(s);
        if (len > 0 && len < 64) {
            char nm[64];
            memcpy(nm, s, len); nm[len] = 0;
            int enemy = (len > 7 && !strcmp(nm + len - 7, "_battle")) || (len > 6 && !strcmp(nm + len - 6, "_logic"));
            if (!enemy || !strcmp(nm, "010000_logic")) {
                snprintf(path, sizeof(path), "ai/aiCommon/%s.luac", nm);
                if (ai_host_load(g_ai_host, path)) { printf("ash-bosstest: lua load failed %s\n", path); return 0; }
                loaded++;
            }
        }
        s = e ? e + 1 : NULL;
        while (s && (*s == '\r' || *s == '\n')) s++;
    }
    if (ai_host_load(g_ai_host, "ai/m18_01_00_00/223200_battle.luac")) return 0;
    if (boss_ai_attach(&g_ai, g_ai_host, b, "ai/think_223200.txt", 0x2232u) < 0) return 0;
    printf("ash-bosstest: LUA MODE - %d aiCommon chunks + 223200_battle on the src/ai host\n", loaded);
    return 1;
}
#endif

PSP_MODULE_INFO("Ash Boss", 0, 0, 1);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);
PSP_HEAP_SIZE_KB(-1024);

#define BUF_WIDTH 512
#define SCR_WIDTH 480
#define SCR_HEIGHT 272
#define DT (1.0f / 30.0f)
#define PI 3.14159265f

static unsigned int __attribute__((aligned(64))) g_list[256 * 1024];
static volatile int g_running = 1;
static int exit_cb(int a, int b, void *c) { (void)a; (void)b; (void)c; g_running = 0; return 0; }
static int cb_thread(SceSize args, void *argp)
{
    (void)args; (void)argp;
    int cb = sceKernelCreateCallback("exit", exit_cb, NULL);
    sceKernelRegisterExitCallback(cb);
    sceKernelSleepThreadCB();
    return 0;
}

static Boss g_b;
static struct {
    Chr chr; AnimSet anim; int idle;
    BonePose pose[ANIM_MAX_BONES];
    ScePspFMatrix4 world[ANIM_MAX_BONES] __attribute__((aligned(16)));
    ScePspFMatrix4 skin[ANIM_MAX_BONES] __attribute__((aligned(16)));
} g_k;

static void dump_frame(void *draw, int n)
{
    char name[64];
    snprintf(name, sizeof(name), "fr_%04d.raw", n);
    SceUID fd = sceIoOpen(name, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0) return;
    sceIoWrite(fd, (void *)(0x44000000 | (u32)draw), BUF_WIDTH * SCR_HEIGHT * 2);
    sceIoClose(fd);
}

/* segment-point distance */
static float seg_dist(const float a[3], const float b[3], const float p[3])
{
    float ab[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, ap[3] = {p[0] - a[0], p[1] - a[1], p[2] - a[2]};
    float l = ab[0] * ab[0] + ab[1] * ab[1] + ab[2] * ab[2];
    float t = l > 1e-6f ? (ab[0] * ap[0] + ab[1] * ap[1] + ab[2] * ap[2]) / l : 0;
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    float d[3] = {a[0] + ab[0] * t - p[0], a[1] + ab[1] * t - p[1], a[2] + ab[2] * t - p[2]};
    return sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
}

static int load(const char *name, u8 **f)
{
    int n = ash_load_file(name, f, NULL, 0);
    if (n < 0) printf("ash-bosstest: %s missing\n", name);
    return n;
}

int main(void)
{
    int th = sceKernelCreateThread("cb_thread", cb_thread, 0x11, 0xFA0, 0, 0);
    if (th >= 0) sceKernelStartThread(th, 0, 0);

    Scene sc;
    u8 *f, *fa, *fb, *fr = NULL;
    int n = load("scene.ashm", &f);
    if (n < 0 || scene_load(&sc, f, n) < 0) { sceKernelExitGame(); return 0; }
    n = load("knight.ashc", &f);
    int na = load("knight.ashanim", &fa);
    if (n < 0 || na < 0 || chr_load(&g_k.chr, f, n) < 0 || anim_load(&g_k.anim, fa, na) < 0) {
        sceKernelExitGame(); return 0;
    }
    g_k.idle = anim_find(&g_k.anim, "idle");
    n = load("boss.ashc", &f);
    na = load("boss.ashanim", &fa);
    int nb = load("boss.ashboss", &fb);
    int nr = load("boss_name.ashtx", &fr);
    int rc = n < 0 || na < 0 || nb < 0 ? -100 : boss_load(&g_b, f, n, fa, na, fb, nb, fr, nr > 0 ? nr : 0);
    if (rc < 0) { printf("ash-bosstest: boss_load failed %d\n", rc); sceKernelExitGame(); return 0; }
    printf("ash-bosstest: boss '%s' hp=%d atk=%u windows=%u dummies=%u clips=%u regions=%u name_tex=%dx%d\n",
           g_b.hdr->name, g_b.hp_max, (unsigned)g_b.hdr->n_atk, (unsigned)g_b.hdr->n_win, (unsigned)g_b.hdr->n_dmy,
           (unsigned)g_b.anim.hdr->n_clips, (unsigned)g_b.hdr->n_reg, g_b.name_w, g_b.name_h);
    printf("ash-bosstest: boss mesh bones=%u chunks=%u tris=%u; start (%.2f %.2f %.2f) yaw %.0f\n",
           (unsigned)g_b.chr.hdr->n_bone, (unsigned)g_b.chr.hdr->n_chunk, (unsigned)(g_b.chr.hdr->n_idx / 3),
           g_b.pos[0], g_b.pos[1], g_b.pos[2], g_b.yaw * 180 / PI);
    printf("ash-bosstest: free=%u KB\n", (unsigned)(sceKernelTotalFreeMemSize() / 1024));

    int dump_every = 0, frames = 1800;
    {
        u8 *buf = NULL;
        if (ash_load_file("demo.txt", &buf, NULL, 0) > 0) { sscanf((const char *)buf, "%d %d", &dump_every, &frames); free(buf); }
    }

    void *fbp0 = guGetStaticVramBuffer(BUF_WIDTH, SCR_HEIGHT, GU_PSM_5650);
    void *fbp1 = guGetStaticVramBuffer(BUF_WIDTH, SCR_HEIGHT, GU_PSM_5650);
    void *zbp = guGetStaticVramBuffer(BUF_WIDTH, SCR_HEIGHT, GU_PSM_4444);
    sceGuInit();
    sceGuStart(GU_DIRECT, g_list);
    sceGuDrawBuffer(GU_PSM_5650, fbp0, BUF_WIDTH);
    sceGuDispBuffer(SCR_WIDTH, SCR_HEIGHT, fbp1, BUF_WIDTH);
    sceGuDepthBuffer(zbp, BUF_WIDTH);
    sceGuOffset(2048 - (SCR_WIDTH / 2), 2048 - (SCR_HEIGHT / 2));
    sceGuViewport(2048, 2048, SCR_WIDTH, SCR_HEIGHT);
    sceGuDepthRange(65535, 0);
    sceGuScissor(0, 0, SCR_WIDTH, SCR_HEIGHT);
    sceGuEnable(GU_SCISSOR_TEST);
    sceGuDepthFunc(GU_GEQUAL);
    sceGuEnable(GU_DEPTH_TEST);
    sceGuFrontFace(GU_CCW);
    sceGuShadeModel(GU_SMOOTH);
    sceGuEnable(GU_CULL_FACE);
    sceGuEnable(GU_TEXTURE_2D);
    sceGuEnable(GU_CLIP_PLANES);
    sceGuEnable(GU_DITHER);
    {
        ScePspFVector3 ldir = {-0.35f, 0.8f, -0.45f};
        sceGuLight(0, GU_DIRECTIONAL, GU_DIFFUSE, &ldir);
        sceGuLightColor(0, GU_DIFFUSE, 0xff86b2d9);
        sceGuLightMode(GU_SINGLE_COLOR);
        sceGuAmbient(0xff4a4a4a);
        sceGuEnable(GU_LIGHT0);
    }
    sceGuFinish();
    sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);
    sceDisplayWaitVblankStart();
    sceGuDisplay(GU_TRUE);
    pspDebugScreenInitEx((void *)(0x44000000 | (u32)fbp0), PSP_DISPLAY_PIXEL_FORMAT_565, 0);
    pspDebugScreenEnableBackColor(0);

    /* stand-in player: north part of the hall, facing the demon (arena.spawn) */
    float pp[3] = {-3.0f, 198.0f, -12.0f}, pyaw = 0;
    float floor_y = g_b.pos[1];
    BossWorld w = {{0}, 0.4f, 0, NULL, NULL};
    {
        u8 *fc;
        int nc = ash_load_file("arena.pcol", &fc, NULL, 0);
        if (nc > 0 && pcol_load(&g_col, fc, nc) == 0) {
            w.collide = boss_collide; w.ctx = &g_col;
            printf("ash-bosstest: arena.pcol %d B, %u tris: the demon collides with the hall\n", nc,
                   (unsigned)g_col.hdr->n_tri);
        }
    }
#ifdef BOSS_LUA
    g_lua = lua_setup(&g_b);
#endif
    g_b.st = BS_INACTIVE; /* waits above, out of sight, until the player walks in */
    int player_hp = 572, hits_taken = 0, hits_given = 0;
    u32 last_swing = 0;
    void *draw = fbp0;
    int dumps = 0;
    const u32 fog = 0xff1a1714;
    float t_hit_flash = 0;
    const char *phase = "";

    for (int fr_i = 0; fr_i < frames && g_running; fr_i++) {
        /* ---------------- script */
        float cam_eye[3], cam_at[3];
        float lp[3];
        if (fr_i == 20) { boss_enter_drop(&g_b); phase = "drop from the ceiling (a00_9060)"; }
        if (fr_i < 170) {
            phase = fr_i < 20 ? "player enters the hall" : phase;
        } else if (fr_i < 1150) {
            /* fight on the floor: walk toward / around the demon, poke it when close */
            phase = "fight";
            float to = atan2f(-(g_b.pos[0] - pp[0]), -(g_b.pos[2] - pp[2]));
            float dx = g_b.pos[0] - pp[0], dz = g_b.pos[2] - pp[2], d = sqrtf(dx * dx + dz * dz);
            int seg = (fr_i - 170) / 140;
            float want = seg % 3 == 0 ? 4.0f : seg % 3 == 1 ? 9.0f : 6.0f; /* close, mid, far-ish */
            float sp = 3.2f * DT;
            if (d > want + 0.5f) { pp[0] += -sinf(to) * sp; pp[2] += -cosf(to) * sp; }
            else if (d < want - 0.5f) { pp[0] -= -sinf(to) * sp; pp[2] -= -cosf(to) * sp; }
            else { pp[0] += cosf(to) * sp * 0.6f; pp[2] += -sinf(to) * sp * 0.6f; } /* circle */
            pyaw = to;
            if (d < 4.5f && fr_i % 40 == 0 && boss_alive(&g_b)) {
                boss_event(&g_b, BOSS_EV_PLAYER_ATTACK);
#ifdef BOSS_LUA
                if (g_lua) boss_ai_interrupt(&g_ai, BOSS_AI_INT_FIND_ATTACK);
#endif
                boss_take_hit(&g_b, 32, 20, BOSS_HIT_NORMAL); /* [придумал] longsword R1 placeholder */
                hits_given++;
            }
        } else if (fr_i < 1500) {
            /* second fight: the player is on the balcony above (region 1812350) */
            phase = "balcony (3020/3021)";
            pp[0] = -3.2f; pp[1] = 206.8f; pp[2] = -32.0f;
            pyaw = 0;
            if (fr_i == 1150) { boss_reset(&g_b, NULL, 0); g_b.engaged = 1; }
        } else if (fr_i == 1500) {
            /* plunge attack from the balcony: lands on the demon's back */
            pp[0] = g_b.pos[0]; pp[1] = floor_y; pp[2] = g_b.pos[2] + 3.4f;
            boss_take_hit(&g_b, 340, 100, BOSS_HIT_PLUNGE); /* [придумал] plunge damage placeholder */
            phase = "plunge attack";
        } else {
            phase = boss_alive(&g_b) ? "finish it" : "victory";
            float to = atan2f(-(g_b.pos[0] - pp[0]), -(g_b.pos[2] - pp[2]));
            float dx = g_b.pos[0] - pp[0], dz = g_b.pos[2] - pp[2], d = sqrtf(dx * dx + dz * dz);
            if (d > 3.6f) { pp[0] += -sinf(to) * 3.2f * DT; pp[2] += -cosf(to) * 3.2f * DT; }
            pyaw = to;
            if (d < 4.5f && fr_i % 20 == 0 && boss_alive(&g_b)) {
                boss_take_hit(&g_b, 64, 20, BOSS_HIT_NORMAL); /* [придумал] R2 placeholder */
                hits_given++;
            }
        }
        if (fr_i < 1150 || fr_i >= 1500) pp[1] = floor_y;
        /* body push-out: keep the player outside the demon's capsule */
        {
            float dx = pp[0] - g_b.pos[0], dz = pp[2] - g_b.pos[2], d = sqrtf(dx * dx + dz * dz);
            float minr = g_b.hdr->radius + 0.4f;
            if (g_b.st != BS_INACTIVE && g_b.st != BS_DEAD && d < minr && d > 1e-3f && pp[1] < g_b.pos[1] + 3) {
                pp[0] = g_b.pos[0] + dx / d * minr; pp[2] = g_b.pos[2] + dz / d * minr;
            }
        }
        memcpy(w.player, pp, 12);
#ifdef BOSS_LUA
        if (g_lua) boss_ai_tick(&g_ai, &w, DT);
        else
#endif
        boss_tick(&g_b, &w, DT);

        /* the demon's attacks against the player capsule (radius 0.4, 0..1.8 m) */
        BossHit hb[8];
        int nh = boss_hits(&g_b, hb, 8);
        for (int i = 0; i < nh; i++) {
            if (hb[i].swing == last_swing) continue;
            float lo[3] = {pp[0], pp[1] + 0.4f, pp[2]}, hi[3] = {pp[0], pp[1] + 1.4f, pp[2]};
            float d = seg_dist(hb[i].a, hb[i].b, lo);
            float d2 = seg_dist(hb[i].a, hb[i].b, hi);
            if (d2 < d) d = d2;
            if (d <= hb[i].radius + 0.4f && pp[1] < 200.0f) {
                const BossFileAtk *a = boss_atk(&g_b, hb[i].atk);
                last_swing = hb[i].swing;
                hits_taken++;
                player_hp -= a->phys / 2; /* [придумал] placeholder for the combat module's formula */
                t_hit_flash = 0.3f;
                printf("ash-bosstest: f%d player hit by ez %d atk row %d (AtkParam beh %u phys %u stam %u poise %u "
                       "impact %u knock %.1f%s)\n", fr_i, g_b.ez, hb[i].atk, (unsigned)a->behavior, a->phys, a->stam,
                       a->poise, a->impact, a->knock, a->flags & BATK_BLAST ? " blast" : "");
            }
        }
        if (t_hit_flash > 0) t_hit_flash -= DT;

        anim_sample(&g_k.anim, g_k.idle, fr_i * DT, g_k.pose);
        anim_skin_matrices(g_k.anim.n_bones, g_k.chr.parent, g_k.pose, g_k.chr.inv_bind, g_k.world, g_k.skin);

        /* camera: behind the player looking at the demon's chest; low and wide during the drop */
        boss_lock_point(&g_b, lp);
        if (g_b.st == BS_INACTIVE) { lp[0] = g_b.pos[0]; lp[1] = floor_y + 3; lp[2] = g_b.pos[2]; }
        {
            float dx = lp[0] - pp[0], dz = lp[2] - pp[2], d = sqrtf(dx * dx + dz * dz) + 1e-3f;
            float back = fr_i < 170 ? 7.0f : 6.0f, up = fr_i < 170 ? 1.2f : 3.2f;
            if (fr_i >= 1150 && fr_i < 1500) { back = 5.0f; up = 2.5f; }
            cam_eye[0] = pp[0] - dx / d * back; cam_eye[1] = pp[1] + up; cam_eye[2] = pp[2] - dz / d * back;
            cam_at[0] = lp[0]; cam_at[1] = fr_i < 170 ? lp[1] + 2.0f : lp[1] - 1.0f; cam_at[2] = lp[2];
        }

        /* ---------------- draw */
        sceGuStart(GU_DIRECT, g_list);
        sceGuClearColor(fog);
        sceGuClearDepth(0);
        sceGuClear(GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT);
        sceGuFog(30.0f, 110.0f, fog);
        sceGuEnable(GU_FOG);
        sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
        sceGuTexFilter(GU_LINEAR, GU_LINEAR);
        sceGuTexWrap(GU_REPEAT, GU_REPEAT);
        sceGuTexScale(1.0f, 1.0f);
        sceGuTexOffset(0.0f, 0.0f);
        sceGuAlphaFunc(GU_GREATER, 0x80, 0xff);
        ScePspFMatrix4 proj, view, vp;
        sceGumMatrixMode(GU_PROJECTION);
        sceGumLoadIdentity();
        sceGumPerspective(60.0f, 480.0f / 272.0f, 0.3f, 400.0f);
        sceGumStoreMatrix(&proj);
        sceGumMatrixMode(GU_VIEW);
        sceGumLoadIdentity();
        {
            ScePspFVector3 eye = {cam_eye[0], cam_eye[1], cam_eye[2]}, at = {cam_at[0], cam_at[1], cam_at[2]}, up = {0, 1, 0};
            sceGumLookAt(&eye, &at, &up);
        }
        sceGumStoreMatrix(&view);
        ash_mat_mul(&vp, &proj, &view);
        sceGumMatrixMode(GU_MODEL);
        sceGumLoadIdentity();
        int nbat = 0, tris = scene_draw(&sc, &vp, &nbat);
        sceGuEnable(GU_LIGHTING);
        sceGuColor(0xffffffff);
        sceGuAmbientColor(0xffffffff);
        sceGumMatrixMode(GU_MODEL);
        sceGumLoadIdentity();
        {
            ScePspFVector3 t = {pp[0], pp[1], pp[2]};
            sceGumTranslate(&t);
            sceGumRotateY(pyaw);
        }
        tris += chr_draw(&g_k.chr, g_k.skin);
        boss_draw(&g_b);
        sceGuDisable(GU_LIGHTING);
        sceGuDisable(GU_FOG);
        boss_debug_draw(&g_b, &vp);
        boss_hud_draw(&g_b);
        sceGuFinish();
        sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);

        pspDebugScreenSetBase((u32 *)(0x44000000 | (u32)draw));
        pspDebugScreenSetXY(0, 0);
        pspDebugScreenSetTextColor(t_hit_flash > 0 ? 0xff4040ff : 0xff9adfff);
        pspDebugScreenPrintf("ASH BOSS f%-4d %-26s ez %-4d d %4.1fm", fr_i, phase, g_b.ez, g_b.dist);
        pspDebugScreenSetXY(0, 1);
        pspDebugScreenPrintf("player hp %d (hit x%d)  %s", player_hp, hits_taken, g_b.log);
        if (dump_every > 0 && fr_i % dump_every == 0) dump_frame(draw, dumps++);
        if (fr_i % 30 == 0)
            printf("ash-bosstest: f%d st=%d ez=%d clip=%s t=%.2f pos=(%.2f %.2f %.2f) yaw=%.0f dist=%.2f hp=%d/%d "
                   "player=(%.1f %.1f %.1f) tris=%d [emulated]\n", fr_i, g_b.st, g_b.ez,
                   g_b.anim.clips[g_b.clip].name, g_b.t, g_b.pos[0], g_b.pos[1], g_b.pos[2], g_b.yaw * 180 / PI,
                   g_b.dist, g_b.hp, g_b.hp_max, pp[0], pp[1], pp[2], tris);
        sceDisplayWaitVblankStart();
        draw = sceGuSwapBuffers();
    }
    printf("ash-bosstest: done thinks=%d player_hits_taken=%d hits_given=%d boss_hp=%d defeated=%d\n",
           g_b.think_count, hits_taken, hits_given, g_b.hp, boss_defeated(&g_b));
    sceKernelExitGame();
    return 0;
}
