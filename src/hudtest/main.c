/*
 * Ash HUD / progression test EBOOT ("Ash HUD"): the Asylum Demon hall, the knight and the Asylum Demon, driven by
 * a fixed script so every piece of src/progress + src/ui shows up in the headless dumps:
 *   area banner -> damage (red bar + yellow trail) -> Estus (clips 7585/7586/7587, +300 HP) -> test bonfire:
 *   prompt, sit, rest menu, stand -> souls from kills (+N rolling in) -> boss bar "Демон Прибежища" with trail ->
 *   death (clip 6000, "ВЫ ПОГИБЛИ", fade, loading screen) -> respawn at the bonfire, bloodstain glow -> pickup ->
 *   "ВРАГ ПОВЕРЖЕН" -> save to ash.sav (and on start: load ash.sav if present).
 * Gameplay here is fake (HP set by the script, no combat module): this EBOOT tests the progression and the HUD,
 * not the fight. The test bonfire spot in the hall is ours [придумал] - the real ones come from the event module.
 *
 * Files next to the EBOOT: scene.ashm, knight.ashc, knight.ashanim, knight_prg.ashanim, ui.ashui, optional
 * demon.ashc + demon.ashanim; demo.txt "<dump_every> <max_frames>" (raw RGB565 dumps fr_NNNN.raw).
 * Stdout "ash-hud: ..." (PPSSPPHeadless log). scripts/shoot-hud.sh stages, runs and converts.
 */
#include <pspctrl.h>
#include <pspdisplay.h>
#include <pspge.h>
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
#include "chr.h"
#include "scene.h"
#include "ui.h"
#include "hud.h"
#include "progress.h"
#include "prg_hud.h"
#include "save.h"

PSP_MODULE_INFO("Ash HUD", 0, 0, 1);
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
    int cbid = sceKernelCreateCallback("Exit Callback", exit_cb, NULL);
    sceKernelRegisterExitCallback(cbid);
    sceKernelSleepThreadCB();
    return 0;
}

static const float FLOOR_Y = 182.05f;

/* ------------------------------------------------------------------ characters */

typedef struct {
    Chr chr;
    AnimSet anim, prg;     /* base clips (knight.ashanim) + progression clips (knight_prg.ashanim) */
    int has_prg;
    const AnimSet *set, *pset;
    int clip, pclip;
    float t, pt, blend;
    int loop_override;
    float pos[3], yaw;
    BonePose __attribute__((aligned(16))) pa[ANIM_MAX_BONES], pb[ANIM_MAX_BONES];
    ScePspFMatrix4 __attribute__((aligned(16))) world[ANIM_MAX_BONES], skin[ANIM_MAX_BONES];
} Actor;

static Actor g_k, g_d;
static int g_has_demon;

static void act_play(Actor *a, const AnimSet *set, int clip)
{
    if (clip < 0) return;
    if (set == a->set && clip == a->clip) return;
    a->pset = a->set; a->pclip = a->clip; a->pt = a->t;
    a->set = set; a->clip = clip; a->t = 0; a->blend = 0;
}

static int act_play_name(Actor *a, const char *name)
{
    int c = anim_find(&a->anim, name);
    if (c >= 0) { act_play(a, &a->anim, c); return 1; }
    if (a->has_prg && (c = anim_find(&a->prg, name)) >= 0) { act_play(a, &a->prg, c); return 1; }
    printf("ash-hud: clip %s missing\n", name);
    return 0;
}

static float clip_time(const AnimSet *s, int c, float t)
{
    float d = anim_duration(s, c);
    if (s->clips[c].flags & CLIP_LOOP) return d > 0 ? fmodf(t, d) : 0;
    return t > d ? d : t;
}

static void act_tick(Actor *a, float dt, int root_motion)
{
    if (root_motion && a->set) {
        float d[4];
        anim_root_delta(a->set, a->clip, a->t, a->t + dt, d);
        float s = sinf(a->yaw), c = cosf(a->yaw);
        a->pos[0] += c * d[0] + s * d[2];
        a->pos[2] += -s * d[0] + c * d[2];
        a->yaw += d[3];
    }
    a->t += dt; a->pt += dt; a->blend += dt;
}

static void act_pose(Actor *a)
{
    const float BLEND = 5.0f / 30.0f;
    anim_sample(a->set, a->clip, clip_time(a->set, a->clip, a->t), a->pa);
    if (a->pset && a->blend < BLEND) {
        anim_sample(a->pset, a->pclip, clip_time(a->pset, a->pclip, a->pt), a->pb);
        anim_blend(a->set->n_bones, a->pb, a->pa, a->blend / BLEND, a->pa);
    }
    anim_skin_matrices(a->set->n_bones, a->chr.parent, a->pa, a->chr.inv_bind, a->world, a->skin);
}

static void act_draw(Actor *a)
{
    sceGumMatrixMode(GU_MODEL);
    sceGumLoadIdentity();
    ScePspFVector3 t = {a->pos[0], a->pos[1], a->pos[2]};
    sceGumTranslate(&t);
    sceGumRotateY(a->yaw);
    chr_draw(&a->chr, a->skin);
}

static float wrap_pi(float a)
{
    while (a > PI) a -= 2 * PI;
    while (a < -PI) a += 2 * PI;
    return a;
}

/* turn toward (x, z) and walk; returns 1 when there */
static int walk_to(Actor *a, float x, float z, float stop)
{
    float dx = x - a->pos[0], dz = z - a->pos[2], d = sqrtf(dx * dx + dz * dz);
    if (d <= stop) { act_play_name(a, "idle"); return 1; }
    float want = atan2f(-dx, -dz), diff = wrap_pi(want - a->yaw), step = 8.0f * DT;
    a->yaw = wrap_pi(a->yaw + (diff > step ? step : diff < -step ? -step : diff));
    act_play_name(a, "walk");
    return 0;
}

/* ------------------------------------------------------------------ frame dump */

static void dump_frame(void *draw, int n)
{
    char name[64];
    snprintf(name, sizeof(name), "fr_%04d.raw", n);
    SceUID fd = sceIoOpen(name, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0) return;
    sceIoWrite(fd, (void *)(0x44000000 | (u32)draw), BUF_WIDTH * SCR_HEIGHT * 2);
    sceIoClose(fd);
}

/* ------------------------------------------------------------------ save */

static void do_save(const Progress *p)
{
    u8 blob[128];
    int n = prg_save_write(p, blob, sizeof(blob));
    SavePart parts[1] = {{SAVE_TAG_PROGRESS, blob, n}};
    int r = save_write_file(SAVE_PATH, parts, 1);
    printf("ash-hud: save %s -> %d (souls=%d estus=%d stain=%d/%d deaths=%d)\n", SAVE_PATH, r, (int)p->souls,
           (int)p->estus, (int)p->stain_on, (int)p->stain_souls, (int)p->deaths);
}

static void do_load(Progress *p)
{
    static u8 buf[1024];
    SavePart parts[SAVE_MAX_PARTS];
    int n = save_read_file(SAVE_PATH, buf, sizeof(buf), parts, SAVE_MAX_PARTS);
    if (n < 0) { printf("ash-hud: no save loaded (%d)\n", n); return; }
    const SavePart *pp = save_find(parts, n, SAVE_TAG_PROGRESS);
    int r = pp ? prg_save_read(p, pp->data, pp->size) : -99;
    printf("ash-hud: save loaded: parts=%d progress=%d souls=%d estus=%d stain=%d/%d deaths=%d\n", n, r,
           (int)p->souls, (int)p->estus, (int)p->stain_on, (int)p->stain_souls, (int)p->deaths);
}

/* ------------------------------------------------------------------ main */

int main(void)
{
    int th = sceKernelCreateThread("cb_thread", cb_thread, 0x11, 0xFA0, 0, 0);
    if (th >= 0) sceKernelStartThread(th, 0, 0);

    Scene sc;
    u8 *f;
    char path[256];
    int n = ash_load_file("scene.ashm", &f, path, sizeof(path));
    if (n < 0 || scene_load(&sc, f, n) < 0) { printf("ash-hud: scene.ashm missing\n"); sceKernelExitGame(); return 0; }
    n = ash_load_file("knight.ashc", &f, NULL, 0);
    if (n < 0 || chr_load(&g_k.chr, f, n) < 0) { printf("ash-hud: knight.ashc missing\n"); sceKernelExitGame(); return 0; }
    n = ash_load_file("knight.ashanim", &f, NULL, 0);
    if (n < 0 || anim_load(&g_k.anim, f, n) < 0) { printf("ash-hud: knight.ashanim missing\n"); sceKernelExitGame(); return 0; }
    n = ash_load_file("knight_prg.ashanim", &f, NULL, 0);
    g_k.has_prg = n > 0 && anim_load(&g_k.prg, f, n) == 0 && g_k.prg.n_bones == g_k.anim.n_bones;
    printf("ash-hud: knight_prg.ashanim %s (%d clips)\n", g_k.has_prg ? "ok" : "MISSING", g_k.has_prg ? (int)g_k.prg.hdr->n_clips : 0);
    UiRes ui;
    n = ash_load_file("ui.ashui", &f, path, sizeof(path));
    int ui_ok = n > 0 && ui_load(&ui, f, n) == 0;
    printf("ash-hud: %s %d B -> %s, %u glyphs, %u strings, %u images\n", path, n, ui_ok ? "ok" : "BAD",
           ui_ok ? (unsigned)ui.h->n_glyph : 0, ui_ok ? (unsigned)ui.h->n_str : 0, ui_ok ? (unsigned)ui.h->n_img : 0);
    if (!ui_ok) memset(&ui, 0, sizeof(ui));
    else {
        int iw, ih;
        ui_img_size(&ui, IMG_DIED, &iw, &ih);
        printf("ash-hud: ui check: w(\"572\")=%d w(S_REST \"%s\")=%d line_h=%d/%d img_died=%dx%d atlas=%ux%u off=%u\n",
               ui_text_w(&ui, UI_FONT_S, "572"), ui_str(&ui, S_REST), ui_text_w(&ui, UI_FONT_S, ui_str(&ui, S_REST)),
               ui_line_h(&ui, UI_FONT_S), ui_line_h(&ui, UI_FONT_L), iw, ih, ui.h->atlas_w, ui.h->atlas_h,
               (unsigned)ui.h->off_atlas);
    }
    n = ash_load_file("demon.ashc", &f, NULL, 0);
    int dc = n > 0 ? chr_load(&g_d.chr, f, n) : -100, da = -100;
    if (dc == 0) {
        int na = ash_load_file("demon.ashanim", &f, NULL, 0);
        da = na > 0 ? anim_load(&g_d.anim, f, na) : -100;
        g_has_demon = da == 0 && anim_find(&g_d.anim, "idle") >= 0;
    }
    printf("ash-hud: demon: ashc %d B -> %d, ashanim -> %d, present %d\n", n, dc, da, g_has_demon);
    printf("ash-hud: loaded, free=%u KB max block=%u KB\n", (unsigned)(sceKernelTotalFreeMemSize() / 1024),
           (unsigned)(sceKernelMaxFreeMemSize() / 1024));

    int dump_every = 0, max_frames = 1600;
    {
        u8 *buf = NULL;
        if (ash_load_file("demo.txt", &buf, NULL, 0) > 0) { sscanf((const char *)buf, "%d %d", &dump_every, &max_frames); free(buf); }
    }

    /* ---- progression */
    float hp_max = 572, hp = 572;     /* [версия] Knight VIT 14 -> 572 HP; real value: combat module */
    float sp_max = 88, sp = 88;       /* [версия] END 10 -> 88 */
    Progress prog;
    prg_init(&prog, 0);
    prg_bind_hp(&prog, &hp, &hp_max);
    if (g_k.has_prg)
        for (int a = 1; a < PRG_ANIM_COUNT; a++) {
            int c = anim_find(&g_k.prg, prg_anim_name[a]);
            if (c >= 0) prg_set_clip_len(&prog, a, anim_duration(&g_k.prg, c));
        }
    do_load(&prog);
    Hud hud;
    hud_init(&hud, &ui);

    g_k.pos[0] = -3.0f; g_k.pos[1] = FLOOR_Y; g_k.pos[2] = -24.0f; g_k.yaw = PI;
    g_k.clip = anim_find(&g_k.anim, "idle"); g_k.set = &g_k.anim;
    if (g_has_demon) {
        g_d.pos[0] = -3.31f; g_d.pos[1] = 182.11f; g_d.pos[2] = -9.46f; g_d.yaw = 0;
        g_d.set = &g_d.anim; g_d.clip = anim_find(&g_d.anim, "idle");
    }
    const float bonfire[3] = {-7.0f, FLOOR_Y, -20.0f};   /* [придумал] test spot */
    const float spawn[3] = {-6.0f, FLOOR_Y, -21.2f};
    float boss_hp = 2100, boss_hp_max = 2100;           /* NpcParam 223000 MaximumHP = 2100 [проверено] */
    int boss_on = 0;

    /* ---- GU */
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

    void *draw = fbp0;
    const u32 fog_color = 0xff10141a;
    int frame = 0, dumps = 0;
    int phase_logged = -1;
    float cam_yaw = g_k.yaw;
    int stain_was_on = 0;
    u64 t_last;
    sceRtcGetCurrentTick(&t_last);
    double ui_us_sum = 0;
    u32 tick_res = sceRtcGetTickResolution();

    while (g_running && frame < max_frames) {
        /* ================= script (one fixed step per frame) */
        PrgInput in;
        memset(&in, 0, sizeof(in));
        in.pos[0] = g_k.pos[0]; in.pos[1] = g_k.pos[1]; in.pos[2] = g_k.pos[2];
        const char *prompt = 0;
        int fr = frame;
        if (fr == 10) prg_banner(&prog, IMG_AREA_ASYLUM);
        if (!prg_locked(&prog)) {
            if (fr < 120) act_play_name(&g_k, "idle");
            else if (fr < 170) walk_to(&g_k, -4.0f, -20.0f, 0.3f);
            else if (fr < 230) {
                act_play_name(&g_k, "idle");
                if (fr == 175 || fr == 190 || fr == 205) { hp -= 95; sp = 30; }   /* scripted hits */
            } else if (fr == 232) in.item = 1;                                    /* Estus */
            else if (fr > 232 && fr < 400) {
                if (walk_to(&g_k, bonfire[0] + 0.9f, bonfire[2] - 0.7f, 0.4f)) {
                    g_k.yaw = atan2f(-(bonfire[0] - g_k.pos[0]), -(bonfire[2] - g_k.pos[2]));
                    prompt = ui_str(&ui, S_REST);
                    if (fr == 385) { prg_rest_begin(&prog); prg_on_reload(&prog); prompt = 0; }
                }
            } else if (fr >= 560 && fr < 700) {
                if (fr == 580) prg_add_souls(&prog, 20);   /* hollow kills: NpcParam 250020/250022 getSoul 20 */
                if (fr == 600) prg_add_souls(&prog, 20);
                if (fr == 640) prg_add_souls(&prog, 20);
                walk_to(&g_k, -3.5f, -15.5f, 0.4f);
            } else if (fr >= 700 && fr < 820) {
                boss_on = 1;
                g_k.yaw = atan2f(-(g_d.pos[0] - g_k.pos[0]), -(g_d.pos[2] - g_k.pos[2]));
                act_play_name(&g_k, fr % 40 < 30 ? "attack" : "idle");
                if (fr % 40 == 20) boss_hp -= 140;
                if (fr == 760 || fr == 790) hp -= 260;          /* demon's hammer */
                if (fr == 805) { hp = 0; prg_player_died(&prog, g_k.pos); }
            } else if (fr >= 1020 && fr < 1230) {
                if (fr < 1200) walk_to(&g_k, prog.stain_on ? prog.stain_pos[0] : -3.5f, prog.stain_on ? prog.stain_pos[2] : -15.5f, 0.3f);
                else act_play_name(&g_k, "idle");
            } else if (fr >= 1230 && fr < 1450) {
                boss_on = boss_hp > 0;
                if (boss_on) g_k.yaw = atan2f(-(g_d.pos[0] - g_k.pos[0]), -(g_d.pos[2] - g_k.pos[2]));
                if (fr % 25 == 5) boss_hp -= 300;
                act_play_name(&g_k, boss_on && fr % 25 < 18 ? "attack" : "idle");
                if (boss_hp <= 0 && boss_on && fr % 25 == 5) {
                    boss_hp = 0;
                    boss_on = 0;
                    prg_banner(&prog, IMG_VICTORY);
                    prg_add_souls(&prog, 0);   /* NpcParam 223000 SoulReward = 0 [проверено] */
                    printf("ash-hud: f%d boss defeated\n", fr);
                }
            } else act_play_name(&g_k, "idle");
        }
        if (prog.phase == PRG_REST && prog.sub == 1) {         /* menu: down, up, then Leave */
            static int menu_t = 0;
            menu_t++;
            if (menu_t == 20) in.down = 1;
            if (menu_t == 35) in.up = 1;
            if (menu_t == 70) in.confirm = 1;
        }
        if (boss_hp < 0) boss_hp = 0;
        if (fr == 1560) do_save(&prog);
        sp = sp + 40.0f * DT > sp_max ? sp_max : sp + 40.0f * DT;   /* fake regen for the bar */

        prg_tick(&prog, &in, DT);
        if (prog.req & PRG_REQ_ANIM) {
            const char *nm = prg_anim_name[prog.anim];
            act_play_name(&g_k, nm);
            printf("ash-hud: f%d anim %s\n", fr, nm);
        }
        if (prog.req & PRG_REQ_RELOAD) {
            /* what the event module would do: EV_CMD_RELOAD -> refill, respawn at the bonfire */
            prg_on_reload(&prog);
            g_k.pos[0] = spawn[0]; g_k.pos[1] = spawn[1]; g_k.pos[2] = spawn[2];
            g_k.yaw = PI * 0.75f;
            boss_hp = boss_hp_max; boss_on = 0;
            act_play_name(&g_k, "idle");
            printf("ash-hud: f%d reload: souls=%d stain=%d (%d at %.1f %.1f) hp=%.0f estus=%d\n", fr, (int)prog.souls,
                   (int)prog.stain_on, (int)prog.stain_souls, prog.stain_pos[0], prog.stain_pos[2], hp, (int)prog.estus);
        }
        if (prog.req & PRG_REQ_SAVE) do_save(&prog);
        if (prog.req & PRG_REQ_SOULS_BACK) printf("ash-hud: f%d bloodstain picked up, souls=%d\n", fr, (int)prog.souls);
        if (prog.phase != phase_logged) {
            printf("ash-hud: f%d phase %d hp=%.0f estus=%d souls=%d\n", fr, prog.phase, hp, (int)prog.estus, (int)prog.souls);
            phase_logged = prog.phase;
        }
        if (stain_was_on != prog.stain_on) stain_was_on = prog.stain_on;

        int moving = !prg_locked(&prog) && g_k.set == &g_k.anim && g_k.clip == anim_find(&g_k.anim, "walk");
        act_tick(&g_k, DT, moving);
        g_k.pos[1] = FLOOR_Y;
        if (g_has_demon) act_tick(&g_d, DT, 0);

        HudFrame hf;
        hud_frame_clear(&hf);
        prg_fill_hud(&prog, &ui, &hf);
        hf.sp = sp; hf.sp_max = sp_max;
        hf.boss_on = boss_on;
        hf.boss_hp = boss_hp; hf.boss_hp_max = boss_hp_max;
        hf.boss_name = ui_str(&ui, S_BOSS_ASYLUM);
        hf.prompt = prompt;
        hud_tick(&hud, &hf, DT);

        /* ================= draw */
        act_pose(&g_k);
        if (g_has_demon) act_pose(&g_d);
        /* camera: behind-right of the knight, smoothed */
        /* resting: swing round to the side so the sitting knight is not hidden behind the fire glow */
        cam_yaw += wrap_pi(g_k.yaw + (prg_resting(&prog) ? 1.3f : 0.0f) - cam_yaw) * 0.08f;
        float eye[3] = {g_k.pos[0] + sinf(cam_yaw) * 3.6f + cosf(cam_yaw) * 0.6f, g_k.pos[1] + 2.0f,
                        g_k.pos[2] + cosf(cam_yaw) * 3.6f - sinf(cam_yaw) * 0.6f};
        float at[3] = {g_k.pos[0] - sinf(cam_yaw) * 1.5f, g_k.pos[1] + 1.2f, g_k.pos[2] - cosf(cam_yaw) * 1.5f};

        sceGuStart(GU_DIRECT, g_list);
        sceGuClearColor(fog_color);
        sceGuClearDepth(0);
        sceGuClear(GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT);
        sceGuFog(30.0f, 110.0f, fog_color);
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
            ScePspFVector3 e = {eye[0], eye[1], eye[2]}, a = {at[0], at[1], at[2]}, up = {0, 1, 0};
            sceGumLookAt(&e, &a, &up);
        }
        sceGumStoreMatrix(&view);
        ash_mat_mul(&vp, &proj, &view);
        sceGumMatrixMode(GU_MODEL);
        sceGumLoadIdentity();
        int nb = 0;
        scene_draw(&sc, &vp, &nb);
        sceGuEnable(GU_LIGHTING);
        sceGuColor(0xffffffff);
        sceGuAmbientColor(0xffffffff);
        act_draw(&g_k);
        if (g_has_demon) act_draw(&g_d);
        sceGuDisable(GU_LIGHTING);
        sceGuDisable(GU_FOG);

        u64 tu0;
        sceRtcGetCurrentTick(&tu0);
        ui_begin();
        hud_draw_fire(&hud, &vp, bonfire);
        if (prog.stain_on) hud_draw_stain(&hud, &vp, prog.stain_pos);
        hud_draw(&hud, &hf);
        ui_end();
        u64 tu1;
        sceRtcGetCurrentTick(&tu1);
        ui_us_sum += (double)(tu1 - tu0) * 1e6 / tick_res;
        sceGuFinish();
        sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);

        if (dump_every > 0 && frame % dump_every == 0) dump_frame(draw, dumps++);
        sceDisplayWaitVblankStart();
        draw = sceGuSwapBuffers();
        frame++;
    }
    printf("ash-hud: done: %d frames, %d dumps, hud list build avg %.0f us [emulated], souls=%d deaths=%d\n", frame, dumps,
           ui_us_sum / (frame ? frame : 1), (int)prog.souls, (int)prog.deaths);
    sceGuTerm();
    sceKernelExitGame();
    return 0;
}
