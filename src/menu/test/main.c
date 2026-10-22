/*
 * Ash menu test EBOOT ("Ash Menu"): drives src/menu through every screen with a fixed button script and dumps
 * frames (fr_NNNN.raw every <dump_every> frames, sh_<name>.raw at named moments) for scripts/.. (docs/menu.md):
 *   title -> main menu -> character creation (keyboard "Пепел", all 10 classes, hair, gender, gifts, Рыцарь) ->
 *   game (the Asylum hall + the created character) -> START bar -> equipment -> inventory -> status -> settings
 *   (scheme, brightness) -> system dialog -> bonfire (rest, level up) -> "ВЫ ПОГИБЛИ" -> back to the title.
 * Files next to the EBOOT: menu.ashui, menu.ashcls, menu_idle.ashanim, cls_*.ashc, head_*.ashc, scene.ashm (any
 * .ashm - the world behind in-game menus), demo.txt "<dump_every> <max_frames>".
 * Stdout "ash-menu: ..." (PPSSPPHeadless log): events, memory (sceKernelTotalFreeMemSize) per stage.
 */
#include <pspctrl.h>
#include <pspdisplay.h>
#include <pspge.h>
#include <pspgu.h>
#include <pspgum.h>
#include <pspiofilemgr.h>
#include <pspkernel.h>
#include <malloc.h>
#include <sys/types.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "anim.h"
#include "chr.h"
#include "scene.h"
#include "menu.h"

PSP_MODULE_INFO("Ash Menu", 0, 0, 1);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);
PSP_HEAP_SIZE_KB(-1024);

#define BUF_WIDTH 512
#define SCR_WIDTH 480
#define SCR_HEIGHT 272
#define DT (1.0f / 30.0f)

static unsigned int __attribute__((aligned(64))) g_list[256 * 1024];

/* ------------------------------------------------------------------ autopilot script */

enum { OP_WAIT, OP_PRESS, OP_HOLD, OP_SHOT, OP_OPEN, OP_END };
typedef struct { int op; u32 arg; int n; const char *name; } Step;
#define W(n) {OP_WAIT, 0, n, 0}
#define P(b) {OP_PRESS, b, 0, 0}
#define PW(b, n) {OP_PRESS, b, 0, 0}, {OP_WAIT, 0, n, 0}
#define H(b, n) {OP_HOLD, b, n, 0}
#define S(s) {OP_SHOT, 0, 0, s}
#define O(scr) {OP_OPEN, scr, 0, 0}
#define X PSP_CTRL_CROSS
#define C PSP_CTRL_CIRCLE
#define U PSP_CTRL_UP
#define D PSP_CTRL_DOWN
#define L PSP_CTRL_LEFT
#define R PSP_CTRL_RIGHT
#define ST PSP_CTRL_START

static const Step SCRIPT[] = {
    W(55), S("01_title"), PW(X, 22), S("02_main"),
    PW(D, 8), S("03_main_new"), PW(X, 25), S("04_create"),
    /* name: П е п е л */
    PW(X, 12), PW(D, 3), PW(R, 3), PW(R, 3), PW(R, 3), PW(R, 3), PW(R, 3), PW(X, 4),
    PW(U, 3), PW(X, 4), PW(D, 3), PW(X, 4), PW(U, 3), PW(X, 4), PW(D, 3), PW(L, 3), PW(L, 3), PW(L, 3), PW(L, 3),
    PW(X, 10), S("05_osk"), PW(ST, 12),
    /* class list: every class */
    PW(D, 4), PW(D, 4), PW(X, 6), PW(U, 30), S("06_class_0_warrior"),
    PW(D, 30), S("07_class_1_knight"), PW(D, 30), S("08_class_2_wanderer"), PW(D, 30), S("09_class_3_thief"),
    PW(D, 30), S("10_class_4_bandit"), PW(D, 30), S("11_class_5_hunter"), PW(D, 30), S("12_class_6_sorcerer"),
    PW(D, 30), S("13_class_7_pyromancer"), PW(D, 30), S("14_class_8_cleric"), PW(D, 30), S("15_class_9_deprived"),
    PW(X, 8),
    /* hair on the Deprived (male) */
    PW(D, 4), PW(D, 4), PW(X, 8), S("16_hair_m0"), PW(D, 22), S("17_hair_m1"), PW(D, 22), S("18_hair_m2"),
    PW(D, 22), S("19_hair_m3"), PW(U, 4), PW(U, 4), PW(X, 6),
    /* gender -> female */
    PW(U, 4), PW(U, 4), PW(U, 4), PW(X, 6), PW(D, 22), S("20_gender_female"), PW(X, 6),
    /* class -> Hunter (bare head) */
    PW(D, 4), PW(X, 6), PW(U, 4), PW(U, 4), PW(U, 4), PW(U, 22), S("21_female_hunter"), PW(X, 6),
    /* female hair */
    PW(D, 4), PW(D, 4), PW(X, 8), PW(D, 22), S("22_hair_f1"), PW(D, 22), S("23_hair_f2"), PW(D, 22), S("24_hair_f3"),
    PW(U, 4), PW(X, 6),
    /* gifts */
    PW(U, 4), PW(X, 8), S("25_gift_0"), PW(D, 14), S("26_gift_1"), PW(D, 14), S("27_gift_2"), PW(D, 14), S("28_gift_3"),
    PW(D, 14), S("29_gift_4"), PW(D, 14), S("30_gift_5"), PW(D, 14), S("31_gift_6"), PW(D, 14), S("32_gift_7"),
    PW(D, 14), S("33_gift_8"), PW(U, 4), PW(U, 4), PW(U, 4), PW(X, 6),
    /* class -> Knight, gender -> male */
    PW(U, 4), PW(X, 6), PW(U, 4), PW(U, 4), PW(U, 4), PW(U, 4), PW(X, 6),
    PW(U, 4), PW(X, 6), PW(U, 4), PW(X, 20), S("34_final_knight"), H(PSP_CTRL_RTRIGGER, 40), W(4), S("35_turned"),
    /* done */
    PW(D, 4), PW(D, 4), PW(D, 4), PW(D, 4), PW(X, 12), S("36_confirm"), PW(X, 40), S("37_game"),
    /* START bar -> equipment */
    PW(ST, 12), S("38_pause"), PW(X, 12), S("39_equip"), PW(D, 4), PW(D, 4), PW(R, 4), PW(R, 6), S("40_equip_armor"),
    PW(D, 6), S("41_equip_quick"), PW(C, 8),
    /* inventory */
    PW(R, 8), S("42_pause_inv"), PW(X, 12), S("43_inventory"), PW(D, 5), PW(D, 10), S("44_inventory_gift"), PW(C, 8),
    /* status */
    PW(R, 8), PW(X, 12), S("45_status"), PW(C, 8),
    /* settings */
    PW(R, 8), PW(X, 12), S("46_settings"), PW(R, 8), S("47_settings_scheme_b"), PW(D, 4), PW(R, 4), PW(R, 4), PW(R, 4),
    W(6), S("48_settings_bright"), PW(L, 4), PW(L, 4), PW(L, 4), PW(D, 4), PW(R, 8), PW(U, 4), PW(U, 4), PW(L, 6), PW(C, 8),
    /* system -> "В главное меню?" -> no */
    PW(R, 8), PW(X, 10), S("49_system_ask"), PW(C, 6), PW(C, 12),
    /* bonfire */
    O(MENU_BONFIRE), W(14), S("50_bonfire"), PW(X, 8), PW(D, 4), PW(X, 12), S("51_levelup"),
    PW(R, 4), PW(R, 4), PW(D, 4), PW(D, 4), PW(D, 4), PW(R, 8), S("52_levelup_raised"),
    PW(D, 4), PW(D, 4), PW(D, 4), PW(D, 4), PW(D, 6), PW(X, 10), S("53_levelup_ask"), PW(X, 10), S("54_bonfire_after"),
    PW(D, 4), PW(X, 12),
    /* death */
    O(MENU_DIED), W(40), S("55_died"), W(50), S("56_died_full"), W(80),
    /* back to the title */
    PW(ST, 10), PW(L, 8), PW(X, 8), PW(L, 6), S("57_to_title_ask"), PW(X, 50), S("58_title_again"), W(10),
    {OP_END, 0, 0, 0},
};

/* ------------------------------------------------------------------ world (behind in-game menus) */

typedef struct {
    Chr body, head;
    int has_body, has_head;
    AnimSet anim;
    int clip;
    float t;
    BonePose __attribute__((aligned(16))) pose[ANIM_MAX_BONES];
    ScePspFMatrix4 __attribute__((aligned(16))) world[ANIM_MAX_BONES], skin[ANIM_MAX_BONES];
} Player;

static int load_chr(Chr *c, const char *name)
{
    u8 *f;
    int n = ash_load_file(name, &f, 0, 0);
    if (n <= 0 || chr_load(c, f, n) != 0) { printf("ash-menu: test: %s missing\n", name); return 0; }
    return 1;
}

static void dump(void *draw, const char *name)
{
    SceUID fd = sceIoOpen(name, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0) return;
    sceIoWrite(fd, (void *)(0x44000000 | (u32)draw), BUF_WIDTH * SCR_HEIGHT * 2);
    sceIoClose(fd);
}

static const char *ev_name(int ev)
{
    static const char *n[] = {"NONE", "NEW_GAME", "CONTINUE", "QUIT", "CLOSE", "REST", "LEAVE_BONFIRE", "LEVELUP",
                              "TO_TITLE", "SETTINGS", "USE_ITEM", "DIED_DONE"};
    return ev >= 0 && ev < (int)(sizeof(n) / sizeof(n[0])) ? n[ev] : "?";
}

static unsigned heap_used_kb(void)
{
    struct mallinfo mi = mallinfo();   /* newlib heap (PSP_HEAP_SIZE_KB): bytes in use */
    return (unsigned)(mi.uordblks / 1024);
}

static void mem(const char *what, const Menu *m)
{
    printf("ash-menu: mem %-14s heap_used=%u KB menu_files=%u KB\n", what, heap_used_kb(),
           m ? (unsigned)(menu_mem_bytes(m) / 1024) : 0);
}

int main(void)
{
    mem("start", 0);
    static Menu m;
    if (menu_init(&m) != 0) { sceKernelExitGame(); return 0; }
    mem("menu_init", &m);
    Scene sc;
    u8 *f;
    int n = ash_load_file("scene.ashm", &f, 0, 0);
    int has_scene = n > 0 && scene_load(&sc, f, n) == 0;
    printf("ash-menu: scene %s\n", has_scene ? "ok" : "missing");
    int dump_every = 3, max_frames = 4000;
    {
        u8 *buf = NULL;
        if (ash_load_file("demo.txt", &buf, NULL, 0) > 0) { sscanf((const char *)buf, "%d %d", &dump_every, &max_frames); free(buf); }
    }
    menu_set_has_save(&m, 1);   /* test: show "Продолжить" enabled */
    menu_open(&m, MENU_TITLE);

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
    int frame = 0, dumps = 0, step = 0, step_t = 0;
    u32 held = 0;
    int in_game = 0, peak_free_low = 0;   /* = peak heap use at shots */
    static Player pl;
    MenuGameView view;
    const u32 fog = 0xff10141a;
    const float FLOOR_Y = 182.05f;   /* the Asylum hall floor (src/hudtest) */
    float ppos[3] = {-3.0f, FLOOR_Y, -24.0f}, pyaw = 3.14159265f;   /* hudtest spawn */

    while (frame < max_frames) {
        /* ---- script */
        held = 0;
        const Step *s = &SCRIPT[step];
        if (s->op == OP_END) break;
        switch (s->op) {
        case OP_WAIT: if (++step_t >= s->n) { step++; step_t = 0; } break;
        case OP_PRESS: if (step_t < 2) held = s->arg; if (++step_t >= 3) { step++; step_t = 0; } break;
        case OP_HOLD: held = s->arg; if (++step_t >= s->n) { step++; step_t = 0; } break;
        case OP_OPEN: menu_open(&m, (int)s->arg); printf("ash-menu: f%d open %d\n", frame, (int)s->arg); step++; break;
        case OP_SHOT: break;   /* handled after drawing */
        }
        /* ---- game side: START opens the bar when no menu is up */
        if (in_game && !menu_screen(&m) && (held & PSP_CTRL_START) && step_t == 1) {
            menu_open(&m, MENU_PAUSE);   /* the held START is the first tick's "previous" state: no edge */
            printf("ash-menu: f%d open pause\n", frame);
        }
        int ev = menu_tick(&m, held, DT);
        if (ev) printf("ash-menu: f%d event %s (screen %d)\n", frame, ev_name(ev), menu_screen(&m));
        if (ev == MEV_NEW_GAME) {
            printf("ash-menu: created \"%s\" gender=%d class=%d (row %d) gift=%d hair=%d level=%d attr=%d %d %d %d %d %d %d %d\n",
                   m.chr.name, m.chr.gender, m.chr.class_idx, m.chr.class_row, m.chr.gift_idx, m.chr.hair, m.chr.level,
                   m.chr.attr[0], m.chr.attr[1], m.chr.attr[2], m.chr.attr[3], m.chr.attr[4], m.chr.attr[5],
                   m.chr.attr[6], m.chr.attr[7]);
            mem("after_create", &m);
            char nm[32];
            snprintf(nm, sizeof(nm), "cls_%d_%c.ashc", m.chr.class_row, m.chr.gender ? 'f' : 'm');
            pl.has_body = load_chr(&pl.body, nm);
            if (m.cls[m.chr.class_idx].bare_head) {
                snprintf(nm, sizeof(nm), "head_%c_%d.ashc", m.chr.gender ? 'f' : 'm', m.chr.hair);
                pl.has_head = load_chr(&pl.head, nm);
            }
            pl.anim = m.idle;
            pl.clip = m.idle_clip;
            menu_fill_view_from_char(&m, &view);
            view.souls = 3200; view.hp = view.hp_max * 0.7f; view.poise = 22; view.phys_def = 87;
            view.atk_r1 = 112; view.atk_l1 = 58;   /* stub numbers for the screens */
            menu_set_view(&m, &view);
            in_game = 1;
            mem("game", &m);
        }
        if (ev == MEV_REST) { view.hp = view.hp_max; printf("ash-menu: rest -> hp %.0f\n", view.hp); }
        if (ev == MEV_LEVELUP) printf("ash-menu: level up -> level %d souls %d str %d vit %d\n", view.level, view.souls,
                                      view.attr[MA_STR], view.attr[MA_VIT]);
        if (ev == MEV_SETTINGS) printf("ash-menu: settings scheme=%d bright=%d cam=%d inv=%d/%d overlay=%d\n", m.set.scheme,
                                       m.set.brightness, m.set.cam_speed, m.set.invert_x, m.set.invert_y, m.set.overlay);
        if (ev == MEV_TO_TITLE) { in_game = 0; menu_set_view(&m, 0); menu_open(&m, MENU_TITLE); }
        pl.t += DT;

        /* ---- draw */
        sceGuStart(GU_DIRECT, g_list);
        sceGuClearColor(menu_fullscreen(&m) ? 0xff000000 : fog);
        sceGuClearDepth(0);
        sceGuClear(GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT);
        if (in_game && !menu_fullscreen(&m) && has_scene) {
            sceGuFog(30.0f, 110.0f, fog);
            sceGuEnable(GU_FOG);
            sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
            sceGuTexFilter(GU_LINEAR, GU_LINEAR);
            sceGuTexWrap(GU_REPEAT, GU_REPEAT);
            sceGuAlphaFunc(GU_GREATER, 0x80, 0xff);
            ScePspFMatrix4 proj, vw, vp;
            sceGumMatrixMode(GU_PROJECTION);
            sceGumLoadIdentity();
            sceGumPerspective(60.0f, 480.0f / 272.0f, 0.3f, 400.0f);
            sceGumStoreMatrix(&proj);
            sceGumMatrixMode(GU_VIEW);
            sceGumLoadIdentity();
            {
                float cy = pyaw + 3.14159265f - 0.6f;   /* in front-right of the character */
                ScePspFVector3 e = {ppos[0] + sinf(cy) * 3.4f, ppos[1] + 1.9f, ppos[2] + cosf(cy) * 3.4f};
                ScePspFVector3 a = {ppos[0], ppos[1] + 1.1f, ppos[2]}, up = {0, 1, 0};
                sceGumLookAt(&e, &a, &up);
            }
            sceGumStoreMatrix(&vw);
            ash_mat_mul(&vp, &proj, &vw);
            sceGumMatrixMode(GU_MODEL);
            sceGumLoadIdentity();
            int nb = 0;
            scene_draw(&sc, &vp, &nb);
            if (pl.has_body && pl.clip >= 0) {
                anim_sample(&pl.anim, pl.clip, pl.t, pl.pose);
                anim_skin_matrices(pl.anim.n_bones, pl.body.parent, pl.pose, pl.body.inv_bind, pl.world, pl.skin);
                sceGuEnable(GU_LIGHTING);
                sceGuColor(0xffffffff);
                sceGuAmbientColor(0xffffffff);
                sceGumMatrixMode(GU_MODEL);
                sceGumLoadIdentity();
                ScePspFVector3 t = {ppos[0], ppos[1], ppos[2]};
                sceGumTranslate(&t);
                sceGumRotateY(pyaw);
                chr_draw(&pl.body, pl.skin);
                if (pl.has_head) chr_draw(&pl.head, pl.skin);
                sceGuDisable(GU_LIGHTING);
            }
            sceGuDisable(GU_FOG);
            menu_draw_brightness(&m.set);
        }
        menu_draw(&m);
        sceGuFinish();
        sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);

        while (SCRIPT[step].op == OP_SHOT) {
            char nm[64];
            snprintf(nm, sizeof(nm), "sh_%s.raw", SCRIPT[step].name);
            dump(draw, nm);
            int fr = (int)heap_used_kb();
            if (fr > peak_free_low) peak_free_low = fr;
            printf("ash-menu: f%d shot %s screen=%d heap_used=%u KB menu=%u KB loads=%d\n", frame, SCRIPT[step].name,
                   menu_screen(&m), (unsigned)fr, (unsigned)(menu_mem_bytes(&m) / 1024), m.load_count);
            step++;
        }
        if (dump_every > 0 && frame % dump_every == 0) {
            char nm[32];
            snprintf(nm, sizeof(nm), "fr_%04d.raw", dumps++);
            dump(draw, nm);
        }
        sceDisplayWaitVblankStart();
        draw = sceGuSwapBuffers();
        frame++;
    }
    printf("ash-menu: done: %d frames, %d dumps, step %d/%d, peak heap use at shots %d KB\n", frame, dumps, step,
           (int)(sizeof(SCRIPT) / sizeof(SCRIPT[0])) - 1, peak_free_low);
    sceGuTerm();
    sceKernelExitGame();
    return 0;
}
