/*
 * Ash items test EBOOT ("Ash Items"): src/items + src/menu (with src/items/menu-ext-images.patch) on the real cooked
 * data. A Knight starts the Asylum way (hilt, no shield), picks up the m18_01 / m10_02 loot through itm_award_lot,
 * changes equipment, uses items from the inventory screen (MEV_USE_ITEM -> itm_use) and the screens are dumped:
 *   sh_<name>.raw at named moments (src/items/test_psp/shoot.sh -> PNG).
 * Files next to the EBOOT: items.ashit, items.ashui, menu.ashui, menu.ashcls, menu_idle.ashanim, cls_3001_m.ashc,
 * scene.ashm. Stdout "ash-items: ..." (PPSSPPHeadless log).
 */
#include <pspctrl.h>
#include <pspdisplay.h>
#include <pspge.h>
#include <pspgu.h>
#include <pspgum.h>
#include <pspiofilemgr.h>
#include <pspkernel.h>
#include <malloc.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "anim.h"
#include "chr.h"
#include "scene.h"
#include "menu.h"
#include "items.h"
#include "items_menu.h"

PSP_MODULE_INFO("Ash Items", 0, 0, 1);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);
PSP_HEAP_SIZE_KB(-1024);

#define BUF_WIDTH 512
#define SCR_WIDTH 480
#define SCR_HEIGHT 272
#define DT (1.0f / 30.0f)

static unsigned int __attribute__((aligned(64))) g_list[256 * 1024];

enum { OP_WAIT, OP_PRESS, OP_SHOT, OP_OPEN, OP_ACT, OP_END };
typedef struct { int op; u32 arg; int n; const char *name; } Step;
#define W(n) {OP_WAIT, 0, n, 0}
#define PW(b, n) {OP_PRESS, b, 0, 0}, {OP_WAIT, 0, n, 0}
#define S(s) {OP_SHOT, 0, 0, s}
#define O(scr) {OP_OPEN, scr, 0, 0}
#define ACT(k) {OP_ACT, k, 0, 0}
#define X PSP_CTRL_CROSS
#define C PSP_CTRL_CIRCLE
#define U PSP_CTRL_UP
#define D PSP_CTRL_DOWN
#define R PSP_CTRL_RIGHT

enum { A_KEY, A_CLASS_WEAPONS, A_EQUIP_CLASS, A_LOOT, A_EQUIP_MORE, A_BOSS, A_EQUIP_HAMMER, A_CLOSE };

static const Step SCRIPT[] = {
    W(20), O(MENU_EQUIP), W(14), S("01_equip_asylum_start"), PW(C, 8),
    ACT(A_KEY), W(10), S("02_got_cell_key"), W(50),
    ACT(A_CLASS_WEAPONS), W(40),
    ACT(A_EQUIP_CLASS), O(MENU_EQUIP), W(14), S("04_equip_broadsword_tower_shield"), PW(C, 8),
    ACT(A_LOOT), W(10), S("05_got_firelink_loot"), W(40),
    ACT(A_EQUIP_MORE), O(MENU_EQUIP), W(10), PW(D, 4), PW(D, 4), PW(D, 6), S("06_equip_ring_quick"), PW(C, 8),
    O(MENU_INVENTORY), W(12), S("07_inventory"), PW(D, 5), PW(D, 8),
    PW(D, 8), S("09_inventory_soul"), PW(X, 12), S("10_after_use"), PW(C, 8),
    O(MENU_STATUS), W(12), S("11_status"), PW(C, 8),
    ACT(A_BOSS), W(50),
    ACT(A_EQUIP_HAMMER), O(MENU_STATUS), W(12), S("13_status_hammer_r2"), PW(C, 8),
    W(10),
    {OP_END, 0, 0, 0},
};

static void dump(void *draw, const char *name)
{
    SceUID fd = sceIoOpen(name, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0) return;
    sceIoWrite(fd, (void *)(0x44000000 | (u32)draw), BUF_WIDTH * SCR_HEIGHT * 2);
    sceIoClose(fd);
}

static unsigned heap_used_kb(void) { struct mallinfo mi = mallinfo(); return (unsigned)(mi.uordblks / 1024); }

/* ---- game side */
static ItmDb db;
static Inv inv;
static int32_t estus = 0;
static int souls = 1200;
static uint32_t rng = 0x1234567u;
static char toast[256];
static float toast_t = 99;
static MenuGameView view;
static ItmMenuMap map;
static ItmStats st;
static const ItmClass *cls;

static void refresh(Menu *m)
{
    itm_calc(&inv, &db, cls->attr, &st);
    itm_menu_fill(&db, &inv, &st, &view, &map);
    view.souls = souls;
    view.humanity = inv.humanity;
    menu_set_view(m, &view);
    printf("ash-items: stats hp %.0f sp %.0f load %.1f/%.1f roll %d poise %.0f def %.0f AR R %.1f L %.1f items %d\n",
           st.hp_max, st.sp_max, st.load, st.load_max, st.roll, st.poise, st.def[0], st.ar_r[0], st.ar_l[0], view.n_items);
}

static void award(const uint32_t *lots, int n)
{
    ItmGot got[16];
    int k = 0;
    for (int i = 0; i < n; i++) {
        int g = itm_award_lot(&inv, &db, lots[i], &rng, got + k, 16 - k);
        for (int j = 0; j < g; j++) printf("ash-items: lot %u -> %s x%d (added %d)\n", (unsigned)lots[i],
                                           itm_name(&db, got[k + j].key), got[k + j].count, got[k + j].added);
        k += g;
    }
    itm_got_text(&db, got, k, toast, 96);   /* one HUD line */
    toast_t = 0;
    printf("ash-items: toast \"%s\"\n", toast);
}

static void equip_key(int slot, uint32_t key)
{
    int idx = -1;
    for (int i = inv.n - 1; i >= 0; i--) if (inv.e[i].key == key) { idx = i; break; }
    int rc = itm_equip(&inv, &db, slot, idx);
    printf("ash-items: equip slot %d <- %s: %d\n", slot, itm_name(&db, key), rc);
}

static void act(Menu *m, int a)
{
    switch (a) {
    case A_KEY: { static const uint32_t l[] = {1810000}; award(l, 1); break; }
    case A_CLASS_WEAPONS: { static const uint32_t l[] = {1810120, 1810130}; award(l, 2); break; }
    case A_EQUIP_CLASS:
        equip_key(ITM_SLOT_R1, ITM_KEY(0, 202000));
        equip_key(ITM_SLOT_L1, ITM_KEY(0, 1452000));
        equip_key(ITM_SLOT_R2, ITM_KEY(0, 212000));
        break;
    case A_LOOT: {
        static const uint32_t l[] = {1810060, 1810070, 1080, 1020000, 1020110};
        award(l, 5);
        itm_add(&inv, &db, ITM_KEY(4, 200), 1);   /* Oscar's Estus (src/event gives the flask; charges = progress) */
        estus = 5;
        break;
    }
    case A_EQUIP_MORE:
        equip_key(ITM_SLOT_RING1, ITM_KEY(2, 125));
        equip_key(ITM_SLOT_Q1, ITM_KEY(4, 200));
        equip_key(ITM_SLOT_Q2, ITM_KEY(4, 292));
        equip_key(ITM_SLOT_Q3, ITM_KEY(4, 500));
        break;
    case A_BOSS: { static const uint32_t l[] = {2660, 2661}; award(l, 2); break; }
    case A_EQUIP_HAMMER: equip_key(ITM_SLOT_R2, ITM_KEY(0, 852000)); break;
    }
    refresh(m);
}

static int load_chr(Chr *c, const char *name)
{
    u8 *f;
    int n = ash_load_file(name, &f, 0, 0);
    return n > 0 && chr_load(c, f, n) == 0;
}

int main(void)
{
    static Menu m;
    if (menu_init(&m) != 0) { printf("ash-items: menu_init failed\n"); sceKernelExitGame(); return 0; }
    u8 *f;
    int n = ash_load_file("items.ashit", &f, 0, 0);
    int rc = n > 0 ? itm_db_load(&db, f, n) : -100;
    printf("ash-items: items.ashit %d B rc %d items %u lots %u\n", n, rc, rc ? 0u : (unsigned)db.h->n_item, rc ? 0u : (unsigned)db.h->n_lot);
    if (rc) { sceKernelExitGame(); return 0; }
    static UiRes icons;
    u8 *fi;
    int ni = ash_load_file("items.ashui", &fi, 0, 0);
    int ri = ni > 0 ? ui_load(&icons, fi, ni) : -100;
    printf("ash-items: items.ashui %d B rc %d icons %u\n", ni, ri, ri ? 0u : (unsigned)icons.h->n_img);
    if (!ri) menu_set_ext_images(&m, &icons);
    printf("ash-items: mem heap_used=%u KB (menu + items)\n", heap_used_kb());

    cls = itm_find_class(&db, 3001);   /* Knight */
    itm_new_game(&inv, &db, 3001, 0, 1);
    inv.estus = &estus;
    memset(&view, 0, sizeof view);
    view.name = "Пепел";
    view.class_name = menu_dstr(&m, m.cls[1].s_name);
    view.level = cls->level;
    memcpy(view.attr, cls->attr, 8);
    m.chr.class_idx = 1;
    refresh(&m);

    Scene sc;
    n = ash_load_file("scene.ashm", &f, 0, 0);
    int has_scene = n > 0 && scene_load(&sc, f, n) == 0;
    static Chr body;
    int has_body = load_chr(&body, "cls_3001_m.ashc");
    static BonePose __attribute__((aligned(16))) pose[ANIM_MAX_BONES];
    static ScePspFMatrix4 __attribute__((aligned(16))) world[ANIM_MAX_BONES], skin[ANIM_MAX_BONES];

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
    int frame = 0, step = 0, step_t = 0;
    const u32 fog = 0xff10141a;
    const float FLOOR_Y = 182.05f;
    float ppos[3] = {-3.0f, FLOOR_Y, -24.0f}, pyaw = 3.14159265f, t = 0;

    while (frame < 3000) {
        u32 held = 0;
        const Step *s = &SCRIPT[step];
        if (s->op == OP_END) break;
        switch (s->op) {
        case OP_WAIT: if (++step_t >= s->n) { step++; step_t = 0; } break;
        case OP_PRESS: if (step_t < 2) held = s->arg; if (++step_t >= 3) { step++; step_t = 0; } break;
        case OP_OPEN: menu_open(&m, (int)s->arg); step++; break;
        case OP_ACT: act(&m, (int)s->arg); step++; break;
        default: break;
        }
        int ev = menu_tick(&m, held, DT);
        if (ev == MEV_USE_ITEM && m.ev_arg >= 0 && m.ev_arg < map.n) {
            int idx = map.inv_idx[m.ev_arg];
            uint32_t key = inv.e[idx].key;
            ItmUse u;
            int r = itm_use(&inv, &db, idx, &u);
            printf("ash-items: use %s -> rc %d kind %d value %d consumed %d\n", itm_name(&db, key), r, u.kind, u.value,
                   u.consumed);
            if (r == ITM_OK && u.kind == ITM_USE_SOULS) souls += u.value;
            if (r == ITM_OK && u.kind == ITM_USE_ESTUS && estus > 0) estus--;
            if (r == ITM_OK) { itm_used_text(&db, key, toast, sizeof toast); toast_t = 0; }
            refresh(&m);
            printf("ash-items: souls now %d\n", souls);
        }
        t += DT;
        toast_t += DT;

        sceGuStart(GU_DIRECT, g_list);
        sceGuClearColor(fog);
        sceGuClearDepth(0);
        sceGuClear(GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT);
        if (has_scene) {
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
                float cy = pyaw + 3.14159265f - 0.6f;
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
            if (has_body && m.idle_clip >= 0) {
                anim_sample(&m.idle, m.idle_clip, t, pose);
                anim_skin_matrices(m.idle.n_bones, body.parent, pose, body.inv_bind, world, skin);
                sceGuEnable(GU_LIGHTING);
                sceGuColor(0xffffffff);
                sceGuAmbientColor(0xffffffff);
                sceGumMatrixMode(GU_MODEL);
                sceGumLoadIdentity();
                ScePspFVector3 tr = {ppos[0], ppos[1], ppos[2]};
                sceGumTranslate(&tr);
                sceGumRotateY(pyaw);
                chr_draw(&body, skin);
                sceGuDisable(GU_LIGHTING);
            }
            sceGuDisable(GU_FOG);
        }
        menu_draw(&m);
        if (toast_t < 2.5f && toast[0]) {   /* "Получено: ..." banner (the game's HUD draws its own; test only) */
            ui_begin();
            int w = ui_text_w(&m.ui, 1, toast) + 40;
            ui_rect(240 - w / 2, 200, w, 30, 0xc0000000);
            ui_frame(240 - w / 2, 200, w, 30, 0xff3c6a8c);
            ui_text_sh(&m.ui, 1, 240, 206, toast, 0xffd8e8f0, UI_CENTER);
            ui_end();
        }
        sceGuFinish();
        sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);
        while (SCRIPT[step].op == OP_SHOT) {
            char nm[64];
            snprintf(nm, sizeof(nm), "sh_%s.raw", SCRIPT[step].name);
            dump(draw, nm);
            printf("ash-items: f%d shot %s screen=%d heap_used=%u KB\n", frame, SCRIPT[step].name, menu_screen(&m),
                   heap_used_kb());
            step++;
        }
        sceDisplayWaitVblankStart();
        draw = sceGuSwapBuffers();
        frame++;
    }
    printf("ash-items: done: %d frames, step %d/%d\n", frame, step, (int)(sizeof(SCRIPT) / sizeof(SCRIPT[0])) - 1);
    sceGuTerm();
    sceKernelExitGame();
    return 0;
}
