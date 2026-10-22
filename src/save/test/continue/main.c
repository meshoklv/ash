/*
 * Ash "Продолжить" demo EBOOT (src/save/test/continue, own build dir build/savecont): src/menu + src/save together,
 * across emulator restarts (scripts/test-save-continue.sh):
 *   run 1, no save on the memory stick: title -> main menu ("Продолжить" grey) -> Новая игра -> name "Пепел",
 *     a class from the list -> game -> bonfire menu -> "Отдохнуть" = autosave (sceUtilitySavedata, async, frames keep going)
 *   run 2, new emulator: boot reads the save -> main menu with "Продолжить" active -> X -> MEV_CONTINUE -> the saved
 *     character back in the menu's status and equipment screens.
 * Files next to the EBOOT: menu.ashui, menu.ashcls, menu_idle.ashanim, cls_*.ashc, head_*.ashc.
 * Named frame dumps sh_<name>.raw; stdout "ash-save: ...".
 */
#include <pspctrl.h>
#include <pspdisplay.h>
#include <pspge.h>
#include <pspgu.h>
#include <pspgum.h>
#include <pspiofilemgr.h>
#include <pspkernel.h>
#include <stdio.h>
#include <string.h>

#include "menu.h"
#include "savegame.h"
#include "savestore.h"
#include "save_menu.h"

PSP_MODULE_INFO("Ash Save Continue", 0, 0, 1);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);
PSP_HEAP_SIZE_KB(-1024);

#define BUF_WIDTH 512
#define SCR_WIDTH 480
#define SCR_HEIGHT 272
#define DT (1.0f / 30.0f)

static unsigned int __attribute__((aligned(64))) g_list[256 * 1024];

enum { OP_WAIT, OP_PRESS, OP_SHOT, OP_BONFIRE, OP_UNTIL_SAVED, OP_END };
typedef struct { int op; u32 arg; int n; const char *name; } Step;
#define W(n) {OP_WAIT, 0, n, 0}
#define PW(b, n) {OP_PRESS, b, 0, 0}, {OP_WAIT, 0, n, 0}
#define S(s) {OP_SHOT, 0, 0, s}
#define X PSP_CTRL_CROSS
#define C PSP_CTRL_CIRCLE
#define U PSP_CTRL_UP
#define D PSP_CTRL_DOWN
#define L PSP_CTRL_LEFT
#define R PSP_CTRL_RIGHT
#define ST PSP_CTRL_START

/* run 1: new game, bonfire rest = autosave (name keys as in src/menu/test: "Пепел") */
static const Step NEW_GAME[] = {
    W(55), S("1_01_title"), PW(X, 22), S("1_02_main_no_save"), PW(X, 25),
    PW(X, 12), PW(D, 3), PW(R, 3), PW(R, 3), PW(R, 3), PW(R, 3), PW(R, 3), PW(X, 4),
    PW(U, 3), PW(X, 4), PW(D, 3), PW(X, 4), PW(U, 3), PW(X, 4), PW(D, 3), PW(L, 3), PW(L, 3), PW(L, 3), PW(L, 3),
    PW(X, 10), PW(ST, 12),
    PW(D, 4), PW(D, 4), PW(X, 6), PW(D, 30), PW(X, 8),          /* class list, one down (the run picked Странник 3002) */
    PW(D, 4), PW(D, 4), PW(D, 4), PW(X, 12), S("1_03_create_confirm"), PW(X, 40),
    {OP_BONFIRE, 0, 0, 0}, W(14), S("1_04_bonfire"), PW(X, 2), W(2), S("1_05_saving"), {OP_UNTIL_SAVED, 0, 0, 0},
    W(4), S("1_06_saved"), W(10), {OP_END, 0, 0, 0},
};
/* run 2: continue */
static const Step CONTINUE[] = {
    W(55), S("2_01_title"), PW(X, 22), S("2_02_main_continue"), PW(X, 30), S("2_03_status"),
    W(4), {OP_END, 0, 0, 0},
};

static void dump(void *draw, const char *name)
{
    char nm[64];
    snprintf(nm, sizeof(nm), "sh_%s.raw", name);
    SceUID fd = sceIoOpen(nm, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0) return;
    sceIoWrite(fd, (void *)(0x44000000 | (u32)draw), BUF_WIDTH * SCR_HEIGHT * 2);
    sceIoClose(fd);
}

static SaveGame save;
static SaveStore store;

int main(void)
{
    static Menu m;
    if (menu_init(&m) != 0) { printf("ash-save: menu files missing\n"); sceKernelExitGame(); return 0; }
    /* boot: is there a save? (the game does the same before the title) */
    sv_init(&store, SV_BACKEND_ALL, 0);
    int has = sv_read_sync(&store, &save) == SV_OK;
    printf("ash-save: boot read: %s (from %s, seq %u) name \"%s\" level %d souls %d\n", has ? "save found" : "no save",
           sv_src_name(store.last_src), (unsigned)save.seq, save.name, save.level, (int)save.souls);
    menu_set_has_save(&m, has);
    menu_open(&m, MENU_TITLE);
    const Step *script = has ? CONTINUE : NEW_GAME;

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
    int frame = 0, step = 0, step_t = 0, saving = 0, saved = 0;
    static MenuGameView view;
    while (frame < 3000) {
        u32 held = 0;
        const Step *s = &script[step];
        if (s->op == OP_END) break;
        switch (s->op) {
        case OP_WAIT: if (++step_t >= s->n) { step++; step_t = 0; } break;
        case OP_PRESS: if (step_t < 2) held = s->arg; if (++step_t >= 3) { step++; step_t = 0; } break;
        case OP_BONFIRE: menu_open(&m, MENU_BONFIRE); step++; break;
        case OP_UNTIL_SAVED: if (saved) step++; break;
        default: break;
        }
        int ev = menu_tick(&m, held, DT);
        if (ev == MEV_NEW_GAME) {
            /* the game starts: character from the menu, a little progress so the restore is visible */
            sg_init(&save);
            sg_from_menu_char(&save, &m.chr);
            sg_from_settings(&save, &m.set);
            menu_fill_view_from_char(&m, &view);
            save.area = 18; save.block = 1;
            save.souls = 2750; save.humanity = 3; save.play_s = 754;
            save.hp_max = view.hp_max; save.hp = view.hp_max - 61;
            save.estus = 4; save.estus_max = 5; save.deaths = 2;
            menu_set_view(&m, &view);
            menu_close(&m);
            printf("ash-save: f%d new game \"%s\" class row %d level %d\n", frame, save.name, save.class_row, save.level);
        }
        if (ev == MEV_REST && !saving) {
            int r = sv_write_begin(&store, &save);   /* bonfire rest = autosave, frames keep running */
            saving = r == SV_BUSY;
            printf("ash-save: f%d rest -> autosave begin %d\n", frame, r);
        }
        if (saving) {
            int r = sv_poll(&store);
            if (r != SV_BUSY) {
                saving = 0;
                saved = 1;
                printf("ash-save: f%d autosave %s -> %s seq %u (%d polls), err 0x%08x\n", frame, r == SV_OK ? "OK" : "ERR",
                       sv_src_name(store.last_src), (unsigned)save.seq, store.frames, (unsigned)store.last_err);
            }
        }
        if (ev == MEV_CONTINUE) {
            MenuSettings set = m.set;
            sg_to_menu_char(&save, &m.chr);
            sg_to_settings(&save, &set);
            m.set = set;
            menu_fill_view_from_char(&m, &view);
            sg_to_view(&save, &view);
            menu_set_view(&m, &view);
            menu_open(&m, MENU_STATUS);
            printf("ash-save: f%d continue: \"%s\" class row %d level %d souls %d humanity %d hp %.0f/%.0f zone %s, "
                   "settings scheme %d bright %d\n", frame, save.name, save.class_row, save.level, (int)save.souls,
                   (int)save.humanity, save.hp, save.hp_max, sg_zone_name(save.area, save.block), set.scheme,
                   set.brightness);
        }

        sceGuStart(GU_DIRECT, g_list);
        sceGuClearColor(0xff10141a);
        sceGuClearDepth(0);
        sceGuClear(GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT);
        menu_draw(&m);
        sceGuFinish();
        sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);
        while (script[step].op == OP_SHOT) {
            dump(draw, script[step].name);
            printf("ash-save: f%d shot %s screen %d\n", frame, script[step].name, menu_screen(&m));
            step++;
        }
        sceDisplayWaitVblankStart();
        draw = sceGuSwapBuffers();
        frame++;
    }
    printf("ash-save: done %d frames\n", frame);
    sceGuTerm();
    sceKernelExitGame();
    return 0;
}
