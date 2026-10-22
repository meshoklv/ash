/*
 * Ash "Asylum" EBOOT (docs/asylum.md): the first location of DS1 on the PSP - Northern Undead Asylum m18_01 from the
 * cell to the crow. Simulation: asy_game.c (+ asy_auto.c autopilot); this file = PSP side: GU set-up, drawing of the
 * zone / characters / effects / HUD, the pad, frame dumps for headless runs.
 *
 * Files next to the EBOOT (or host0:/, ms0:/ash/): zone_m18_01.pak, m18_01.ashevt, combat.ashcb, knight.ashc,
 * knight.ashanim, knight_combat.ashanim, knight_prg.ashanim, hollow.ashc, hollow.ashanim, boss.ashc, boss.ashanim,
 * boss.ashboss, boss_name.ashtx, ui.ashui, ai/ (DSR AI bytecode repacked to 32-bit float, scripts/make-dist.sh);
 * demo.txt "<dump_every> <max_frames> <route|hollow|death|boss>" = autopilot (headless has no pad): every Nth frame is
 * written as raw RGB565 fr_NNNN.raw, the game exits after max_frames or when the scenario is over.
 *
 * Controls (the controls design notes scheme A): stick = move, X tap = roll / X hold = run, [] = R1, /\ = R2 (heavy),
 * R (hold) = shield, R + /\ = parry, L = lock-on, O = Estus, d-pad up = action (doors, items, bonfire, fog),
 * menu: up/down + X, O = back. SELECT+START = scheme B. HOME = exit.
 * Stdout "ash-asy: ..." (PPSSPPHeadless log). Emulator timings are not hardware timings.
 */
#include <pspctrl.h>
#include <pspdebug.h>
#include <pspdisplay.h>
#include <pspge.h>
#include <pspgu.h>
#include <pspgum.h>
#include <pspiofilemgr.h>
#include <pspkernel.h>
#include <psppower.h>
#include <psprtc.h>
#include <malloc.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "asy.h"
#include "prg_hud.h"
#include "scene.h"
#ifdef ASY_RENDER
#include "render.h"
#include "render_chrf.h"
#include "render_world.h"
#endif
#ifdef ASH_PERF
#include "perf.h"
#else
#define perf_begin(s) ((void)0)
#define perf_end(s) ((void)0)
#define perf_count_chars(n) ((void)0)
#endif

PSP_MODULE_INFO("Ash Asylum", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);
PSP_HEAP_SIZE_KB(-1024);

#define BUF_WIDTH 512
#define SCR_WIDTH 480
#define SCR_HEIGHT 272

static unsigned int __attribute__((aligned(64))) g_list[512 * 1024];
static volatile int g_running = 1;
static int exit_cb(int a, int b, void *c) { (void)a; (void)b; (void)c; g_running = 0; return 0; }
static int cb_thread(SceSize args, void *argp)
{
    (void)args; (void)argp;
    int cbid = sceKernelCreateCallback("Exit Callback", exit_cb, NULL);
    sceKernelRegisterExitCallback(cbid);
    sceKernelSleepThreadCB();
    return 0;
}

static Game G;
#ifdef ASY_RENDER
static int g_render_on = 1;
#else
static int g_render_on = 0;
#endif   /* demo.txt 4th word "plain" = without src/render (A/B check) */

#ifdef ASY_RENDER
/* PSP speed path (the perf-night design notes): strips/cached lists/sorted textures/mips/hot VRAM for the zone, compact s16
   skinned characters. demo.txt 4th word "slow" = the old world_draw/chr_draw (A/B in one EBOOT). */
static int g_fast = 1;
static RenderWorld g_rw;
static RenderMips g_wmips;
static ChrFast g_cf[3];
static const Chr *g_cf_src[3];
static void chr_draw_any(const Chr *c, const ScePspFMatrix4 *skin)
{
    if (g_fast)
        for (int i = 0; i < 3; i++)
            if (g_cf_src[i] == c) { render_chrf_draw(&g_cf[i], skin); return; }
    chr_draw(c, skin);
}
static void boss_draw_any(Boss *b)
{
    if (!g_fast) { boss_draw(b); return; }
    if (!b->visible || b->st == BS_INACTIVE || b->st == BS_DEAD) return;   /* = boss_draw() with chr_draw_any */
    sceGumMatrixMode(GU_MODEL);
    sceGumLoadIdentity();
    ScePspFVector3 t = {b->pos[0], b->pos[1], b->pos[2]};
    sceGumTranslate(&t);
    sceGumRotateY(b->yaw);
    chr_draw_any(&b->chr, b->skin);
    sceGumLoadIdentity();
}
#else
#define chr_draw_any chr_draw
#define boss_draw_any boss_draw
#endif

/* GE/CPU overlap (the perf-night design notes): the next frame's simulation runs between sceGuFinish and sceGuSync, while
   the GE draws this frame (the list holds copies of every matrix and per-frame vertex, nothing the step changes is
   read by the GE). Same step sequence and input latency as before. demo.txt "nopipe"/"slow" = old order. */
static int g_pipe = 1, g_pipe_steps, g_pipe_demo;
static void sim_steps(int steps, int demo);

static int load(const char *name, u8 **out)
{
    return ash_load_file(name, out, NULL, 0);
}

static u32 g_tick_res;
static u64 now_ticks(void) { u64 t; sceRtcGetCurrentTick(&t); return t; }
static float us_since(u64 t0) { return (float)(now_ticks() - t0) * 1e6f / (float)g_tick_res; }

static void dump_frame(void *draw, int n)
{
    char name[64];
    snprintf(name, sizeof(name), "fr_%04d.raw", n);
    SceUID fd = sceIoOpen(name, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0) return;
    sceIoWrite(fd, (void *)(0x44000000 | (u32)draw), BUF_WIDTH * SCR_HEIGHT * 2);
    sceIoClose(fd);
}

#ifdef ASY_RENDER
static int floor_cb(void *c, float x, float z, float yt, float yb, float *h)
{
    Game *g = (Game *)c;
    return g->has_col && pcol_floor(&g->C, x, z, yt, yb, 0.5f, h) >= 0;
}
#endif

static void draw_body(const Chr *c, const ScePspFMatrix4 *skin, const float pos[3], float yaw)
{
    sceGumMatrixMode(GU_MODEL);
    sceGumLoadIdentity();
    ScePspFVector3 t = {pos[0], pos[1], pos[2]};
    sceGumTranslate(&t);
    sceGumRotateY(yaw);
#ifdef ASY_RENDER
    if (g_render_on) {
        float chest[3] = {pos[0], pos[1] + 1.2f, pos[2]};
        render_char_light_begin(chest);
        chr_draw_any(c, skin);
        render_char_light_end();
        sceGuEnable(GU_LIGHTING);
        return;
    }
#endif
    chr_draw_any(c, skin);
}

/* small lock-on dot at the projected target point */
static void draw_lock_marker(const ScePspFMatrix4 *vp, const float p[3])
{
    float sx, sy, dp;
    if (!ui_project(vp, p, &sx, &sy, &dp)) return;
    ui_rect(sx - 3, sy - 3, 7, 7, 0xff202020);
    ui_rect(sx - 2, sy - 2, 5, 5, 0xffe8f4ff);
}

static void draw_frame(Game *g, void *draw, float fps, int demo)
{
    Player *p = &g->pl;
    sceGuStart(GU_DIRECT, g_list);
    ScePspFMatrix4 proj, view, vp;
    sceGumMatrixMode(GU_PROJECTION);
    sceGumLoadIdentity();
    sceGumPerspective(g->cam.row_fov > 10 ? g->cam.row_fov : 43.0f, 480.0f / 272.0f, 0.3f, 400.0f);
    sceGumStoreMatrix(&proj);
    sceGumMatrixMode(GU_VIEW);
    sceGumLoadIdentity();
    {
        ScePspFVector3 eye = {g->cam.eye[0], g->cam.eye[1], g->cam.eye[2]};
        ScePspFVector3 at = {g->cam.at[0], g->cam.at[1], g->cam.at[2]};
        ScePspFVector3 up = {0, 1, 0};
        sceGumLookAt(&eye, &at, &up);
    }
    sceGumStoreMatrix(&view);
    ash_mat_mul(&vp, &proj, &view);
#ifdef ASY_RENDER
    if (g_render_on) render_begin_frame(&view, &proj, g->cam.eye, ASY_DT);
#endif
    sceGuClearColor(g->W.fog_color);
    sceGuClearDepth(0);
    sceGuClear(GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT);
    sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
    sceGuTexFilter(GU_LINEAR, GU_LINEAR);
    sceGuTexWrap(GU_REPEAT, GU_REPEAT);
    sceGuAlphaFunc(GU_GREATER, 0x80, 0xff);
    WorldStats st;
    perf_begin(PERF_LIST);
#ifdef ASY_RENDER
    if (g_fast) render_world_draw(&g_rw, &proj, &view, g->cam.eye, &st); else
#endif
    world_draw(&g->W, &proj, &view, g->cam.eye, &st);

    /* characters: GE lighting (one directional + ambient, PLAN §3.6), the map has its light baked */
    sceGuTexScale(1.0f, 1.0f);
    sceGuTexOffset(0.0f, 0.0f);
    sceGuEnable(GU_LIGHTING);
    sceGuColor(0xffffffff);
    sceGuAmbientColor(0xffffffff);
    /* camera pinned against the knight (a column or a wall right behind, the controls design notes §4.5): DS1 fades a character
       touching the camera; here the knight is just not drawn while the eye is within 0.7 m of the pivot, else the
       back of the helmet fills the frame [придумал, bug "black frames in the boss fight", 14.10] */
    if (g->cam.cur_dist > 0.7f) draw_body(&g->kchr, g->pb.skin, p->pos, p->yaw);
    for (int i = 0; i < g->n_en; i++) {
        Enemy *e = &g->en[i];
        if (!e->enabled || (e->dead && e->dead_t > 6.0f)) continue;   /* bodies fade after a while [урезание] */
        if (asy_flat_dist(e->pos, g->cam.eye) > g->W.far_dist) continue;
        draw_body(&g->hchr, e->body.skin, e->pos, e->yaw);
    }
    /* eye inside the demon's body (the knight is pushed only to 1.7 m from its centre, inside the 2.5 m hull, and a
       column can pin the camera there): skip it rather than fill the frame with its inside [придумал, 14.10] */
    int eye_in_boss = 0;
    if (g->has_boss) {
        float ex = g->cam.eye[0] - g->B.pos[0], ez = g->cam.eye[2] - g->B.pos[2], ey = g->cam.eye[1] - g->B.pos[1];
        eye_in_boss = ex * ex + ez * ez < g->B.hdr->radius * g->B.hdr->radius && ey > 0 && ey < g->B.hdr->height * 0.8f;
    }
    if (g->has_boss && g->boss_enabled && !g->boss_hidden && !eye_in_boss) {
#ifdef ASY_RENDER
        if (g_render_on) {
            float chest[3] = {g->B.pos[0], g->B.pos[1] + 3.0f, g->B.pos[2]};
            render_char_light_begin(chest);
            boss_draw_any(&g->B);
            render_char_light_end();
        } else
#endif
        boss_draw_any(&g->B);
    }
    sceGuDisable(GU_LIGHTING);
#ifdef ASY_RENDER
    if (g_render_on) {
    render_shadow_blob(p->pos, 0.55f, floor_cb, g);
    for (int i = 0; i < g->n_en; i++)
        if (g->en[i].enabled && !g->en[i].dead) render_shadow_blob(g->en[i].pos, 0.5f, floor_cb, g);
    if (g->has_boss && g->boss_enabled && !g->boss_hidden && g->B.st != BS_INACTIVE && g->B.st != BS_DEAD)
        render_shadow_blob(g->B.pos, 2.2f, floor_cb, g);
    render_fx_draw();
    render_end_frame();
    }
#endif
    sceGuDisable(GU_FOG);

    /* 2D: world markers + HUD */
    HudFrame hf;
    hud_frame_clear(&hf);
    prg_fill_hud(&g->prg, &g->ui, &hf);
    hf.sp = g->pact.sp;
    hf.sp_max = g->pact.sp_max;
    if (!asy_has_estus(g)) hf.estus_max = -1;   /* no flask before Oscar */
    hf.boss_on = g->boss_bar && g->has_boss && g->B.hp > 0;
    if (g->has_boss) { hf.boss_hp = (float)g->B.hp; hf.boss_hp_max = (float)g->B.hp_max; }
    hf.boss_name = g->ui_ok ? ui_str(&g->ui, S_BOSS_ASYLUM) : "";
    hf.prompt = (prg_locked(&g->prg) || p->dead) ? NULL : ev_prompt(&g->ev);
    if (g->toast_text && hf.toast < 0) {
        hf.toast_text = g->toast_text;
        float t = g->toast_t;
        hf.toast_a = t < 0.3f ? t / 0.3f : t > 2.4f ? (3.0f - t) / 0.6f : 1;
    }
    if (g->ev_fade > hf.fade) hf.fade = g->ev_fade;
    hud_tick(&g->hud, &hf, ASY_DT);
    ui_begin();
    for (u32 i = 0; i < g->W.z->n_obj; i++)   /* bonfires: fire glow (+ particles with src/render) */
        if (g->W.obj[i].kind == WK_BONFIRE) {
            float fp[3] = {g->W.obj_m[i][9], g->W.obj_m[i][10] + 0.4f, g->W.obj_m[i][11]};
            if (asy_flat_dist(fp, g->cam.eye) < 40 && !g_render_on) hud_draw_fire(&g->hud, &vp, fp);
        }
    for (int i = 0; i < (int)g->ev.n_treas; i++) {   /* items on the floor: a soft white glint */
        float tp[3], sx, sy, dp;
        if (!ev_treasure_get(&g->ev, i, tp)) continue;
        tp[1] += 0.3f;
        if (asy_flat_dist(tp, g->cam.eye) < 25 && ui_project(&vp, tp, &sx, &sy, &dp))
            ui_glow(sx, sy, 6 + 2 * sinf(g->time * 4 + i), 0x80d0f0ff);
    }
    for (int i = 0; i < ASY_MAX_BOMB; i++) {   /* firebombs in the air */
        const Bomb *b = &g->bomb[i];
        if (!b->on) continue;
        float k = b->t / b->len, bp[3], sx, sy, dp;
        for (int a = 0; a < 3; a++) bp[a] = b->p0[a] + (b->p1[a] - b->p0[a]) * k;
        bp[1] += 3.0f * k * (1 - k) * 4;
        if (ui_project(&vp, bp, &sx, &sy, &dp)) ui_glow(sx, sy, 8, 0xc02080ff);
    }
    if (g->prg.stain_on) hud_draw_stain(&g->hud, &vp, g->prg.stain_pos);
    if (g->lock >= 0) draw_lock_marker(&vp, g->lock_pt);
    hud_draw(&g->hud, &hf);
    if (g->end_demo) {   /* "Конец демо" (the crow takes the Chosen Undead - cut-scene replaced by this screen) */
        float a = g->end_t / 1.5f;
        if (a > 1) a = 1;
        ui_rect(0, 0, UI_W, UI_H, ((u32)(a * 255) << 24));
        if (g->ui_ok && a > 0.5f) {
            ui_text_sh(&g->ui, UI_FONT_L, UI_W / 2, 110, "Конец демо", 0xffe0e8f0, UI_CENTER);
            ui_text_sh(&g->ui, UI_FONT_S, UI_W / 2, 140, ui_str(&g->ui, S_AREA_ASYLUM), 0xffa0b0c0, UI_CENTER);
        }
    }
    ui_end();
    perf_end(PERF_LIST);
    perf_count_chars(1 + g->n_en + (g->has_boss ? 1 : 0));
    sceGuFinish();
    if (g_pipe) sim_steps(g_pipe_steps, g_pipe_demo);
    sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);

    if (demo) {   /* autopilot info on the bottom line (debug font) */
        pspDebugScreenSetBase((u32 *)(0x44000000 | (u32)draw));
        pspDebugScreenSetXY(0, 33);
        pspDebugScreenSetTextColor(0xff9adfff);
        pspDebugScreenPrintf("AUTO %s f%u st %-8s %.0ffps tri %d", auto_scenario_name(g->au.scenario), (unsigned)g->frame,
                             asy_pst_name[p->st], fps, st.tris);
    }
}

static void sim_steps(int steps, int demo)
{
    perf_begin(PERF_SIM);
    for (int s = 0; s < steps; s++) {
        RawPad raw = {0, 128, 128};
        if (!demo) {
            SceCtrlData pad;
            sceCtrlPeekBufferPositive(&pad, 1);
            raw.buttons = pad.Buttons; raw.lx = pad.Lx; raw.ly = pad.Ly;
        }
        game_step(&G, &raw);
#ifdef ASY_RENDER
        for (int k = 0; k < G.n_fx; k++) {
            if (G.fx_kind[k] == 3) fx_roll_dust(G.fx_pos[k], G.pl.yaw);
            else fx_hit_spark(G.fx_pos[k], G.fx_dir[k], G.fx_kind[k] == 2);
        }
#endif
    }
    perf_end(PERF_SIM);
}

int main(void)
{
    int th = sceKernelCreateThread("cb_thread", cb_thread, 0x11, 0xFA0, 0, 0);
    if (th >= 0) sceKernelStartThread(th, 0, 0);
    scePowerSetClockFrequency(333, 333, 166);
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);
    g_tick_res = sceRtcGetTickResolution();
    printf("ash-asy: boot, free=%u KB max block=%u KB\n", (unsigned)(sceKernelTotalFreeMemSize() / 1024),
           (unsigned)(sceKernelMaxFreeMemSize() / 1024));

    u64 tl = now_ticks();
    if (game_load(&G, load) < 0) { printf("ash-asy: load failed\n"); sceKernelExitGame(); return 0; }
    {
        struct mallinfo mi = mallinfo();
        printf("ash-asy: loaded in %.0f ms [emulated]; heap in use %d KB of an arena %d KB (PSP_HEAP_SIZE_KB(-1024) "
               "takes the rest of user RAM), kernel free %u KB\n", us_since(tl) / 1000.0f, mi.uordblks / 1024,
               mi.arena / 1024, (unsigned)(sceKernelTotalFreeMemSize() / 1024));
    }

    int dump_every = 0, max_frames = 0, scenario = ASY_SC_NONE;
    {
        u8 *buf = NULL;
        if (ash_load_file("demo.txt", &buf, NULL, 0) > 0) {
            char sc[32] = "", opt[32] = "";
            sscanf((const char *)buf, "%d %d %31s %31s", &dump_every, &max_frames, sc, opt);
            if (!strcmp(opt, "plain")) g_render_on = 0;
#ifdef ASY_RENDER
            if (!strcmp(opt, "slow")) g_fast = 0;
#endif
            if (!strcmp(opt, "slow") || !strcmp(opt, "nopipe")) g_pipe = 0;
            if (!strcmp(opt, "camlog")) G.camlog = 1;
            if (!strcmp(opt, "doorfight")) G.doorfight = 1;
            scenario = ASY_SC_ROUTE;
            for (int i = 1; i < ASY_SC_COUNT; i++) if (!strcmp(sc, auto_scenario_name(i))) scenario = i;
            free(buf);
        }
    }
    float vd = anim_selftest_vfpu();
    printf("ash-asy: VFPU selftest max diff %.2g -> %s\n", vd, anim_vfpu() ? "VFPU" : "C fallback");

    void *fbp0 = guGetStaticVramBuffer(BUF_WIDTH, SCR_HEIGHT, GU_PSM_5650);
    void *fbp1 = guGetStaticVramBuffer(BUF_WIDTH, SCR_HEIGHT, GU_PSM_5650);
    void *zbp = guGetStaticVramBuffer(BUF_WIDTH, SCR_HEIGHT, GU_PSM_4444);
    {
        int used = 3 * BUF_WIDTH * SCR_HEIGHT * 2;
        int free_vram = sceGeEdramGetSize() - used;
        u8 *vram = (u8 *)sceGeEdramGetAddr() + used;
#ifdef ASY_RENDER
        if (g_fast) {
            u8 *mb;
            int ms = ash_load_file("zone_m18_01.ashmip", &mb, NULL, 0), has_mips = ms > 0 && render_mips_load(&g_wmips, mb, ms) == 0;
            int r = render_world_init(&g_rw, &G.W, has_mips ? &g_wmips : NULL, RW_ALL);
            int got = r == 0 ? render_world_vram(&g_rw, vram, free_vram) : 0;
            const Chr *cs[3] = {&G.kchr, &G.hchr, G.has_boss ? &G.B.chr : NULL};
            for (int i = 0; i < 3; i++) {
                if (!cs[i] || render_chrf_init(&g_cf[i], cs[i]) != 0) continue;
                g_cf_src[i] = cs[i];
                got += render_chrf_vram(&g_cf[i], vram + got, free_vram - got);
                printf("ash-asy: fast chr %d: %d -> %d indices, weights %d -> %d, +%d KB\n", i, g_cf[i].idx_in,
                       g_cf[i].idx_out, g_cf[i].w_in, g_cf[i].w_out, g_cf[i].bytes / 1024);
            }
            printf("ash-asy: fast zone %d (mips %d), lists %d KB, strips %d KB; VRAM %d KB of %d KB\n", r, has_mips,
                   g_rw.dl_bytes / 1024, g_rw.strip_bytes / 1024, got / 1024, free_vram / 1024);
        } else
#endif
        {
        int got = world_upload_vram(&G.W, vram, free_vram);
        printf("ash-asy: zone textures %s VRAM (%d KB of %d KB)\n", got ? "in" : "NOT in", got / 1024, free_vram / 1024);
        }
    }
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
#ifdef ASY_RENDER
    render_init();
    g_render.flags = RF_CHARLIGHT | RF_SHADOWS | RF_FX;   /* zone fog/textures stay src/world's */
    fx_reset();
    for (u32 i = 0; i < G.W.z->n_obj; i++)
        if (G.W.obj[i].kind == WK_BONFIRE) {
            float fp[3] = {G.W.obj_m[i][9], G.W.obj_m[i][10] + 0.15f, G.W.obj_m[i][11]};
            fx_add_fire(fp, 0.45f, 1);
        }
#endif

    game_start(&G, scenario);
    {
        struct mallinfo mi = mallinfo();
        printf("ash-asy: started, heap in use %d KB\n", mi.uordblks / 1024);
    }

    void *draw = fbp0;
    int demo = scenario != ASY_SC_NONE;
    unsigned frame = 0, dumps = 0, frames_in_sec = 0;
    float fps = 0, acc = 0, end_wait = 0;
    double sum_sim = 0, sum_draw = 0;
    double win_sim = 0, win_max = 0;      /* status-line window (150 frames): sim time avg/max, camera casts */
    unsigned win_casts0 = 0, win_cmax = 0, win_n = 0;
    u64 t_prev = now_ticks(), t_last = t_prev;
    while (g_running) {
        u64 tnow = now_ticks();
        acc += (float)(tnow - t_last) / (float)g_tick_res;
        t_last = tnow;
        int steps = demo ? 1 : (int)(acc / ASY_DT);
        if (steps > 2) steps = 2;
        if (!demo) acc -= steps * ASY_DT; else acc = 0;
        if (acc > ASY_DT) acc = ASY_DT;
        u64 ts = now_ticks();
        unsigned casts0 = G.cam_casts;
        if (g_pipe) { g_pipe_steps = steps; g_pipe_demo = demo; }   /* stepped inside draw_frame, GE busy */
        else sim_steps(steps, demo);
        {
            double us = us_since(ts);
            sum_sim += us;
            win_sim += us;
            if (us > win_max) win_max = us;
            if (G.cam_casts - casts0 > win_cmax) win_cmax = G.cam_casts - casts0;
            win_n++;
        }
        u64 td = now_ticks();
        draw_frame(&G, draw, fps, demo);
        sum_draw += us_since(td);
        if (demo && dump_every > 0 && G.frame % dump_every == 0) {
            /* camlog: camera line of every dump in tools/world_render.py form (offline replay of a frame) */
            if (G.camlog)
                printf("DUMPCAM %d f%u %.2f %.2f %.2f %.2f %.2f %.2f %.0f %s\n", dumps, (unsigned)G.frame,
                       G.cam.eye[0], G.cam.eye[1], G.cam.eye[2], G.cam.at[0], G.cam.at[1], G.cam.at[2],
                       G.cam.row_fov > 10 ? G.cam.row_fov : 43.0f,
                       G.W.cur_col >= 0 ? G.W.col[G.W.cur_col].name : "*");
            dump_frame(draw, dumps++);
        }
        sceDisplayWaitVblankStart();
        draw = sceGuSwapBuffers();
        frame++;
        frames_in_sec++;
        u64 tn = now_ticks();
        if (tn - t_prev >= g_tick_res) {
            fps = (float)frames_in_sec * (float)g_tick_res / (float)(tn - t_prev);
            frames_in_sec = 0;
            t_prev = tn;
        }
        if (G.camlog && G.boss_bar && (G.frame % 15 == 0 || G.cam.hit_frac < 1.0f || G.cam.cur_dist < 1.5f)) {
            const CamState *c = &G.cam;
            float bx = c->eye[0] - G.B.pos[0], bz = c->eye[2] - G.B.pos[2];
            printf("CAM f%u st=%s L%d pos %.2f %.2f %.2f piv %.2f %.2f %.2f eye %.2f %.2f %.2f d %.2f/%.2f hit %.2f "
                   "pit %.1f yaw %.1f boss %.2f %.2f %.2f eb %.2f ey %.2f src %.2f %.2f %.2f %.2f cp %.0f cy %.0f\n", (unsigned)G.frame, asy_pst_name[G.pl.st],
                   G.lock, G.pl.pos[0], G.pl.pos[1], G.pl.pos[2], c->pivot[0], c->pivot[1], c->pivot[2], c->eye[0],
                   c->eye[1], c->eye[2], c->cur_dist, c->row_dist + c->frame_extra, c->hit_frac, c->pitch * 57.3f,
                   c->yaw * 57.3f, G.B.pos[0], G.B.pos[1], G.B.pos[2], sqrtf(bx * bx + bz * bz), c->eye[1] - G.B.pos[1],
                   G.cam_dbg[0], G.cam_dbg[1], G.cam_dbg[2], G.cam_dbg[3],
                   c->col_pitch * 57.3f, c->col_yaw * 57.3f);
        }
        if (G.frame % 150 == 0)
            printf("ash-asy: f%u t=%.0fs st=%s pos=(%.2f %.2f %.2f) hp=%.0f/%.0f sp=%.0f souls=%d estus=%d step=%d "
                   "boss=%d/%d st%d cell=%s fps=%.1f sim=%.1fms draw=%.1fms win sim=%.1f/%.1fms casts=%.1f/%u [emulated]\n", (unsigned)G.frame, G.time,
                   asy_pst_name[G.pl.st], G.pl.pos[0], G.pl.pos[1], G.pl.pos[2], G.pact.hp, G.pact.hp_max, G.pact.sp,
                   (int)G.prg.souls, (int)G.prg.estus, G.au.step, G.has_boss ? G.B.hp : 0, G.has_boss ? G.B.hp_max : 0,
                   G.has_boss ? (int)G.B.st : -1, G.W.cur_col >= 0 ? G.W.col[G.W.cur_col].name : "*", fps,
                   sum_sim / frame / 1000.0, sum_draw / frame / 1000.0, win_n ? win_sim / win_n / 1000.0 : 0.0,
                   win_max / 1000.0, win_n ? (double)(G.cam_casts - win_casts0) / win_n : 0.0, win_cmax);
        if (G.frame % 150 == 0) { win_sim = win_max = 0; win_casts0 = G.cam_casts; win_cmax = win_n = 0; }
        if (demo && G.au.done) { end_wait += ASY_DT; if (end_wait > 1.5f) break; }
        if (demo && max_frames > 0 && (int)G.frame >= max_frames) break;
    }
    printf("ASY:{\"scenario\":\"%s\",\"frames\":%u,\"t\":%.1f,\"done\":%d,\"end_demo\":%d,\"victory\":%d,"
           "\"deaths\":%d,\"kills\":%d,\"reloads\":%d,\"saves\":%d,\"rests\":%d,\"bonfires\":%d,\"items\":%d,"
           "\"parries\":%d,\"ripostes\":%d,\"blocks\":%d,\"iframe_dodges\":%d,\"hits_given\":%d,\"hits_taken\":%d,"
           "\"plunges\":%d,\"estus\":%d,\"souls\":%d,\"warps\":%d,\"falls\":%d,\"boss_hp\":%d,\"dumps\":%u,\"heap_kb\":%u}\n",
           auto_scenario_name(scenario), (unsigned)G.frame, G.time, G.au.done, G.end_demo, G.victory, G.deaths,
           G.kills, G.reloads, G.saves, G.rests, G.bonfires_lit, G.items, G.parries, G.ripostes, G.blocks,
           G.rolls_iframe, G.hits_given, G.hits_taken, G.plunges, G.estus_used, (int)G.prg.souls, G.au.warps, G.au.falls,
           G.has_boss ? G.B.hp : -1, dumps, (unsigned)(mallinfo().uordblks / 1024));
    sceGuTerm();
    sceKernelExitGame();
    return 0;
}
