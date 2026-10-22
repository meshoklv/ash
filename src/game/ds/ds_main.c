/*
 * "Ash: Dark Souls" - the full game EBOOT (docs/game.md): one program from the title to Firelink Shrine.
 *
 *   title / main menu / character creation (src/menu, 10 classes) -> Northern Undead Asylum m18_01 (asy_game.c: zone,
 *   events, hollows on the DSR Lua AI, the Asylum Demon, DS1 combat incl. the plunge, Estus/souls/bloodstain) with the
 *   chosen class's model, kit and numbers (src/items) -> src/menu screens over the game (pause, inventory, equipment,
 *   status, settings, bonfire, level up, "ВЫ ПОГИБЛИ") -> the crow -> loading screen -> Firelink Shrine m10_02 (zone
 *   without an event script: bonfire light/rest, the Crestfallen Warrior's FMG lines, loot from ItemLotParam, the
 *   exits walled off - fl_* below; Asylum unloaded first) -> autosave at bonfires, on zone changes and when quitting
 *   to the title (src/save: savedata A/B + file) -> "Продолжить" brings back zone, bonfire, inventory, flags.
 *
 * Drawing = asy_main.c with the PSP speed path of src/render (strips, cached lists, mips, hot VRAM, s16 characters,
 * GE/CPU overlap; the perf-night design notes) and the cheap look set RF_PSP_DEFAULT (docs/render.md §7).
 *
 * Files next to the EBOOT: those of asy_main.c + items.ashit, items.ashui, menu.ashui, menu.ashcls, menu_idle.ashanim,
 * cls_*.ashc, head_*.ashc, hollow_combat.ashanim, zone_m18_01.ashmip, zone_m10_02.pak/.ashmip, crestfallen.ashc/.ashanim,
 * firelink.txt (tools/cook_fltext.py). scripts/make-dist.sh stages them.
 *
 * demo.txt "<dump_every> <max_frames> <full|hollow|boss|death|continue|firelink|flrun> [slow|nopipe|rec=LO-HI]" = autopilot (headless has no
 * pad): a script presses the menu buttons, the Asylum part is driven by asy_auto.c, Firelink by the walk script here.
 * Named frames sh_<name>.raw (RGB565 512x272), every dump_every-th frame fr_NNNN.raw. Stdout "ash-ds: ...".
 * Emulator timings are not hardware timings.
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
#include "diag.h"
#include "items_menu.h"
#include "menu.h"
#include "prg_hud.h"
#include "save_menu.h"
#include "savegame.h"
#include "savestore.h"
#include "scene.h"
#include "render.h"
#include "render_chrf.h"
#include "render_world.h"
#ifdef ASH_PERF
#include "perf.h"
#else
#define perf_begin(s) ((void)0)
#define perf_end(s) ((void)0)
#define perf_count_chars(n) ((void)0)
#endif

PSP_MODULE_INFO("Ash Dark Souls", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);
/* Heap = all free user memory but 4 MB left to the system (savedata utility modules + threads load into the user
   partition on the PSP). In this pspsdk a NEGATIVE PSP_HEAP_SIZE_KB ignores its number and keeps only
   PSP_HEAP_THRESHOLD_SIZE_KB free (default 512 KB): the old PSP_HEAP_SIZE_KB(-1024) left 512 KB, not 1 MB
   [проверено: _sbrk disassembly + "free=..." in the emulator log] (docs/hw-pitfalls.md). */
PSP_HEAP_SIZE_KB(-1);
PSP_HEAP_THRESHOLD_SIZE_KB(4096);

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
static Menu M;
static MenuGameView V;
static ItmMenuMap IM;
static UiRes g_icons;
static int g_icons_ok;
static SaveStore ST;
static SaveGame SG;
static int g_has_save, g_saving, g_save_dirty;

enum { S_TITLE, S_PLAY };
static int g_state = S_TITLE;
static int g_in_game;            /* a zone is loaded */
static int g_fast = 1;
static int g_pipe = 1, g_pipe_steps;
static RenderWorld g_rw;
static int g_rw_on;
static RenderMips g_wmips;
static u8 *g_wmips_file;
static ChrFast g_cf[4];          /* body, hollow, demon, Crestfallen Warrior */
static const Chr *g_cf_src[4];
static u8 *g_vram;
static int g_vram_bytes;
static void *g_draw;
static u32 g_frame;              /* drawn frames */
static int g_dump_every, g_max_frames, g_dumps;
static int g_rec = 1, g_rec_lo = -1, g_rec_hi = -1;   /* fr_ dumps only while g_rec (O_REC) and inside rec=LO-HI (opt) */
static char g_shot_next[48];     /* named frame to dump after this frame's draw */

static int load(const char *name, u8 **out)
{
    diag_stage_t("load file", 20000);   /* one big .pak from a slow stick can take seconds */
    int n = ash_load_file(name, out, NULL, 0);
    if (n <= 0) diag_event("load %s FAILED (%d)", name, n);
    diag_stage("loaded");
    return n;
}

static void dump_raw(const char *name)
{
    SceUID fd = sceIoOpen(name, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0) return;
    sceIoWrite(fd, (void *)(0x44000000 | (u32)g_draw), BUF_WIDTH * SCR_HEIGHT * 2);
    sceIoClose(fd);
}

static int B_active(void);
static void shot(const char *name)
{
    if (!B_active()) return;   /* named frames only under the autopilot */
    snprintf(g_shot_next, sizeof g_shot_next, "%s", name);
}

/* ------------------------------------------------------------------ Firelink Shrine (m10_02, no event script) */

#define FL_MAX_TR 16
#define FL_MAX_SAY 12
#define FL_FLAG_LIT (1u << 16)
#define FL_FLAG_TALKED (1u << 17)
typedef struct {
    int loaded;                  /* model + texts (once) */
    Chr chr;
    AnimSet anim;
    BonePose __attribute__((aligned(16))) pose[ANIM_MAX_BONES];
    ScePspFMatrix4 __attribute__((aligned(16))) world[ANIM_MAX_BONES], skin[ANIM_MAX_BONES];
    int has_npc;
    float npc[4];                /* Crestfallen Warrior (MSB c0000_0018, entity 6270): x y z yaw */
    float bonfire[3];
    int lit;
    struct { float pos[3]; u32 lot; int taken; } tr[FL_MAX_TR];
    int n_tr;
    int exit_reg[8], n_exit;
    float exit_t;
    int say_i;                   /* next line */
    float say_t;                 /* s the current line is shown, <0 none */
    int say_cur;
    char txt[2048];
    const char *area, *talk, *loading, *closed, *say[FL_MAX_SAY];
    int n_say;
    float rest_t;                /* rest started: reload after 1 s */
} Fl;
static Fl FL;

static void fl_texts(void)
{
    u8 *f;
    int n = load("firelink.txt", &f);
    FL.area = "Храм Огня"; FL.talk = "?"; FL.loading = "..."; FL.closed = "-";   /* fallbacks (file missing) */
    if (n <= 0) { printf("ash-ds: firelink.txt missing\n"); return; }
    int o = 0;
    for (int i = 0; i < n && o < (int)sizeof FL.txt - 2; i++) {
        if (f[i] == '\\' && i + 1 < n && f[i + 1] == 'n') { FL.txt[o++] = '\n'; i++; }
        else if (f[i] == '\n') FL.txt[o++] = 0;
        else if (f[i] != '\r') FL.txt[o++] = (char)f[i];
    }
    FL.txt[o] = 0;
    free(f);
    char *line = FL.txt, *end = FL.txt + o;
    while (line < end) {
        char *next = line + strlen(line) + 1;
        char *eq = strchr(line, '=');
        if (eq) {
            *eq = 0;
            const char *v = eq + 1;
            if (!strcmp(line, "area")) FL.area = v;
            else if (!strcmp(line, "talk")) FL.talk = v;
            else if (!strcmp(line, "loading")) FL.loading = v;
            else if (!strcmp(line, "closed")) FL.closed = v;
            else if (!strncmp(line, "say", 3) && FL.n_say < FL_MAX_SAY) FL.say[FL.n_say++] = v;
        }
        line = next;
    }
}

static void fl_pose_npc(void)
{
    int c = anim_find(&FL.anim, "sit");
    if (c < 0) c = 0;
    anim_sample(&FL.anim, c, 0, FL.pose);   /* first frame of a00_7810 (MSB default animation), held [урезание] */
    anim_skin_matrices(FL.anim.n_bones, FL.chr.parent, FL.pose, FL.chr.inv_bind, FL.world, FL.skin);
}

static void fl_load_once(void)
{
    if (FL.loaded) return;
    FL.loaded = 1;
    fl_texts();
    u8 *f;
    int n = load("crestfallen.ashc", &f);
    FL.has_npc = n > 0 && chr_load(&FL.chr, f, n) == 0;
    if (FL.has_npc) {
        n = load("crestfallen.ashanim", &f);
        FL.has_npc = n > 0 && anim_load(&FL.anim, f, n) == 0;
    }
    if (FL.has_npc) fl_pose_npc();
    printf("ash-ds: Firelink: %d lines of the Crestfallen Warrior, model %s\n", FL.n_say, FL.has_npc ? "ok" : "MISSING");
}

static void fl_toast(Game *g, const char *s)
{
    snprintf(g->toast_buf, sizeof g->toast_buf, "%s", s);
    g->toast_text = g->toast_buf;
    g->toast_t = 0;
}

static void fl_step(Game *g, float dt)
{
    Player *p = &g->pl;
    g->prompt_text = NULL;
    int free_ = !prg_locked(&g->prg) && !p->dead && p->st != P_FOG && p->st != P_LADDER;
    int act = free_ && (g->in.pressed & ACT_INTERACT) != 0;
    if (FL.say_t >= 0) { FL.say_t += dt; if (FL.say_t > 4.5f) FL.say_t = -1; }
    if (FL.rest_t > 0 && (FL.rest_t -= dt) <= 0) {   /* sat down: refill, enemies (none here), autosave */
        game_reload_now(g);
        if (g->save_fn) g->save_fn(g, "rest");
    }
    /* the seated warrior is a body the player cannot walk through [версия: radius 0.6 m] */
    if (FL.has_npc && fabsf(p->pos[1] - FL.npc[1]) < 1.5f) {
        float dx = p->pos[0] - FL.npc[0], dz = p->pos[2] - FL.npc[2], r = 0.95f, l = sqrtf(dx * dx + dz * dz);
        if (l < r && l > 1e-3f) { p->pos[0] = FL.npc[0] + dx / l * r; p->pos[2] = FL.npc[2] + dz / l * r; }
    }
    /* bonfire: light, then rest (the menu of src/menu opens once progress sits down) */
    float bd = asy_flat_dist(p->pos, FL.bonfire);
    if (bd < 2.7f && fabsf(p->pos[1] - FL.bonfire[1]) < 2.0f) {
        g->prompt_text = ui_str(&g->ui, FL.lit ? S_REST : S_LIGHT);
        if (act) {
            if (!FL.lit) {
                FL.lit = 1;
                SG.flags |= FL_FLAG_LIT;
                prg_banner(&g->prg, IMG_BONFIRE_LIT);
                g->bonfires_lit++;
                asy_log(g, "Firelink: BONFIRE LIT");
            } else {
                prg_rest_begin(&g->prg);
                g->rests++;
                FL.rest_t = 1.0f;
                asy_log(g, "Firelink: rest at the bonfire");
            }
            return;
        }
    }
    /* the Crestfallen Warrior: one FMG line per "talk", in order, then from the third line again */
    if (FL.has_npc && asy_flat_dist(p->pos, FL.npc) < 3.0f && fabsf(p->pos[1] - FL.npc[1]) < 2.0f && FL.n_say) {
        if (FL.say_t < 0) g->prompt_text = FL.talk;
        if (act) {   /* while a line is up the button goes on to the next one */
            FL.say_cur = FL.say_i;
            FL.say_t = 0;
            FL.say_i = FL.say_i + 1 < FL.n_say ? FL.say_i + 1 : (FL.n_say > 2 ? 2 : 0);
            SG.flags |= FL_FLAG_TALKED;
            asy_log(g, "Firelink: the Crestfallen Warrior says line %d: %s", FL.say_cur, FL.say[FL.say_cur]);
            return;
        }
    }
    /* loot on corpses / in chests (MSB treasure -> ItemLotParam, src/items) */
    for (int i = 0; i < FL.n_tr; i++) {
        if (FL.tr[i].taken) continue;
        if (asy_flat_dist(p->pos, FL.tr[i].pos) > 2.2f || fabsf(p->pos[1] - FL.tr[i].pos[1]) > 2.5f) continue;
        g->prompt_text = ui_str(&g->ui, S_PICKUP);
        if (act) {
            ItmGot got[8];
            int n = g->has_items ? itm_award_lot(&g->inv, &g->idb, FL.tr[i].lot, &g->rng, got, 8) : 0;
            FL.tr[i].taken = 1;
            SG.flags |= 1u << i;
            char buf[128];
            if (n > 0 && itm_got_text(&g->idb, got, n, buf, 96) > 0) fl_toast(g, buf);
            g->items++;
            game_apply_equipment(g);
            asy_log(g, "Firelink: loot lot %u -> %d entries: %s", (unsigned)FL.tr[i].lot, n, n > 0 ? g->toast_buf : "-");
            return;
        }
        break;
    }
    /* exits: invisible walls in the collision (tools/cook_firelink.py); a message at the wall */
    if (FL.exit_t > 0) FL.exit_t -= dt;
    for (int i = 0; i < FL.n_exit; i++)
        if (world_in_region(&g->W, FL.exit_reg[i], p->pos) && FL.exit_t <= 0) {
            fl_toast(g, FL.closed);
            FL.exit_t = 4.0f;
            asy_log(g, "Firelink: exit %u closed", (unsigned)g->W.reg[FL.exit_reg[i]].entity);
        }
}

/* region / object data of the loaded m10_02 pak -> bonfire, respawn, loot, exits, the warrior */
static void fl_setup_zone(Game *g)
{
    World *w = &g->W;
    FL.n_tr = FL.n_exit = 0;
    FL.say_t = -1;
    FL.rest_t = 0;
    for (u32 i = 0; i < w->z->n_obj; i++)
        if (w->obj[i].kind == WK_BONFIRE) memcpy(FL.bonfire, w->obj[i].pos, 12);
    for (u32 i = 0; i < w->z->n_reg; i++) {
        const ZoneReg *r = &w->reg[i];
        if (r->role == WR_TREASURE && FL.n_tr < FL_MAX_TR && r->param > 0) {
            memcpy(FL.tr[FL.n_tr].pos, r->pos, 12);
            FL.tr[FL.n_tr].lot = (u32)r->param;
            FL.tr[FL.n_tr].taken = (SG.flags >> FL.n_tr) & 1;
            FL.n_tr++;
        } else if (r->role == WR_OTHER && r->param == -2 && FL.n_exit < 8) {
            FL.exit_reg[FL.n_exit++] = (int)i;
        }
    }
    int ci = world_find_chr(w, 6270);
    if (ci >= 0) { memcpy(FL.npc, w->chr[ci].pos, 12); FL.npc[3] = w->chr[ci].yaw; }
    else FL.has_npc = 0;
    FL.lit = (SG.flags & FL_FLAG_LIT) != 0;
    w->fog_near = 25.0f; w->fog_far = 120.0f; w->fog_color = 0xff504846;   /* [придумал] docs/firelink.md §2 look.txt */
    w->far_dist = 82.0f; w->lod_dist = 16.0f; w->decor_dist = 30.0f;
    g->zone_step = fl_step;
    asy_log(g, "Firelink: bonfire (%.2f %.2f %.2f), %d loot spots, %d exits, warrior %s", FL.bonfire[0], FL.bonfire[1],
            FL.bonfire[2], FL.n_tr, FL.n_exit, FL.has_npc ? "seated" : "missing");
}

/* ------------------------------------------------------------------ GPU side of a zone: fast paths, VRAM, effects */

static void gpu_zone_free(void)
{
    if (g_rw_on) { render_world_free(&g_rw); g_rw_on = 0; }
    for (int i = 0; i < 4; i++) if (g_cf_src[i]) { render_chrf_free(&g_cf[i]); g_cf_src[i] = NULL; }
    free(g_wmips_file);
    g_wmips_file = NULL;
}

static void gpu_zone_setup(void)
{
    gpu_zone_free();
    int got = 0;
    const char *mip = G.zone == ASY_ZONE_FIRELINK ? "zone_m10_02.ashmip" : "zone_m18_01.ashmip";
    if (g_fast) {
        int ms = load(mip, &g_wmips_file), has_mips = ms > 0 && render_mips_load(&g_wmips, g_wmips_file, ms) == 0;
        if (!has_mips) { free(g_wmips_file); g_wmips_file = NULL; }
        int r = render_world_init(&g_rw, &G.W, has_mips ? &g_wmips : NULL, RW_ALL);
        g_rw_on = r == 0;
        got = g_rw_on ? render_world_vram(&g_rw, g_vram, g_vram_bytes) : 0;
        const Chr *cs[4] = {&G.kchr, G.has_hollow ? &G.hchr : NULL, G.has_boss ? &G.B.chr : NULL,
                            G.zone == ASY_ZONE_FIRELINK && FL.has_npc ? &FL.chr : NULL};
        for (int i = 0; i < 4; i++) {
            if (!cs[i] || render_chrf_init(&g_cf[i], cs[i]) != 0) continue;
            g_cf_src[i] = cs[i];
            got += render_chrf_vram(&g_cf[i], g_vram + got, g_vram_bytes - got);
        }
        printf("ash-ds: zone %d fast path %d (mips %d), lists %d KB, strips %d KB; VRAM %d KB of %d KB\n", G.zone, r,
               has_mips, g_rw.dl_bytes / 1024, g_rw.strip_bytes / 1024, got / 1024, g_vram_bytes / 1024);
    } else {
        got = world_upload_vram(&G.W, g_vram, g_vram_bytes);
        printf("ash-ds: zone %d textures %s VRAM (%d KB)\n", G.zone, got ? "in" : "NOT in", got / 1024);
    }
    sceKernelDcacheWritebackInvalidateAll();
    /* look: RF_PSP_DEFAULT (fog from the zone, mips, character light, blob shadows, particles) */
    g_render.flags = RF_PSP_DEFAULT;
    g_render.fog.near = G.W.fog_near;
    g_render.fog.far = G.W.fog_far;
    g_render.fog.color = G.W.fog_color;
    g_render.fog.clear = G.W.fog_color;
    fx_reset();
    for (u32 i = 0; i < G.W.z->n_obj; i++)
        if (G.W.obj[i].kind == WK_BONFIRE) {
            float fp[3] = {G.W.obj_m[i][9], G.W.obj_m[i][10] + 0.15f, G.W.obj_m[i][11]};
            fx_add_fire(fp, 0.45f, 1);
        }
}

static void chr_draw_any(const Chr *c, const ScePspFMatrix4 *skin)
{
    if (g_fast)
        for (int i = 0; i < 4; i++)
            if (g_cf_src[i] == c) { render_chrf_draw(&g_cf[i], skin); return; }
    chr_draw(c, skin);
}

static void draw_body(const Chr *c, const ScePspFMatrix4 *skin, const float pos[3], float yaw)
{
    sceGumMatrixMode(GU_MODEL);
    sceGumLoadIdentity();
    ScePspFVector3 t = {pos[0], pos[1], pos[2]};
    sceGumTranslate(&t);
    sceGumRotateY(yaw);
    float chest[3] = {pos[0], pos[1] + 1.2f, pos[2]};
    render_char_light_begin(chest);
    chr_draw_any(c, skin);
    render_char_light_end();
    sceGuEnable(GU_LIGHTING);
}

static int floor_cb(void *c, float x, float z, float yt, float yb, float *h)
{
    Game *g = (Game *)c;
    return g->has_col && pcol_floor(&g->C, x, z, yt, yb, 0.5f, h) >= 0;
}

/* ------------------------------------------------------------------ frame */

static void sim_steps(int steps);
static void ui_box_text(const char *s, float y)
{
    if (!G.ui_ok || !s) return;
    int w = ui_text_w(&G.ui, UI_FONT_S, s) + 24, lines = 1;
    for (const char *c = s; *c; c++) lines += *c == '\n';
    int h = lines * ui_line_h(&G.ui, UI_FONT_S) + 10;
    ui_rect(UI_W / 2 - w / 2, y - 5, w, h, 0xa0000000);
    ui_text_sh(&G.ui, UI_FONT_S, UI_W / 2, y, s, 0xffe8f0f4, UI_CENTER);
}

static void draw_game(int with_menu)
{
    Game *g = &G;
    Player *p = &g->pl;
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
    render_begin_frame(&view, &proj, g->cam.eye, ASY_DT);
    sceGuClearColor(g->W.fog_color);
    sceGuClearDepth(0);
    sceGuClear(GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT);
    sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
    sceGuTexFilter(GU_LINEAR, GU_LINEAR);
    sceGuTexWrap(GU_REPEAT, GU_REPEAT);
    sceGuAlphaFunc(GU_GREATER, 0x80, 0xff);
    WorldStats st;
    perf_begin(PERF_LIST);
    if (g_rw_on) render_world_draw(&g_rw, &proj, &view, g->cam.eye, &st);
    else world_draw(&g->W, &proj, &view, g->cam.eye, &st);
    sceGuTexScale(1.0f, 1.0f);
    sceGuTexOffset(0.0f, 0.0f);
    sceGuEnable(GU_LIGHTING);
    sceGuColor(0xffffffff);
    sceGuAmbientColor(0xffffffff);
    if (g->cam.cur_dist > 0.7f) draw_body(&g->kchr, g->pb.skin, p->pos, p->yaw);
    for (int i = 0; i < g->n_en; i++) {
        Enemy *e = &g->en[i];
        if (!e->enabled || (e->dead && e->dead_t > 6.0f)) continue;
        if (asy_flat_dist(e->pos, g->cam.eye) > g->W.far_dist) continue;
        draw_body(&g->hchr, e->body.skin, e->pos, e->yaw);
    }
    if (g->zone == ASY_ZONE_FIRELINK && FL.has_npc) draw_body(&FL.chr, FL.skin, FL.npc, FL.npc[3]);
    int eye_in_boss = 0;
    if (g->has_boss) {
        float ex = g->cam.eye[0] - g->B.pos[0], ez = g->cam.eye[2] - g->B.pos[2], ey = g->cam.eye[1] - g->B.pos[1];
        eye_in_boss = ex * ex + ez * ez < g->B.hdr->radius * g->B.hdr->radius && ey > 0 && ey < g->B.hdr->height * 0.8f;
    }
    if (g->has_boss && g->boss_enabled && !g->boss_hidden && !eye_in_boss && g->B.visible && g->B.st != BS_INACTIVE &&
        g->B.st != BS_DEAD) {
        float chest[3] = {g->B.pos[0], g->B.pos[1] + 3.0f, g->B.pos[2]};
        sceGumMatrixMode(GU_MODEL);
        sceGumLoadIdentity();
        ScePspFVector3 t = {g->B.pos[0], g->B.pos[1], g->B.pos[2]};
        sceGumTranslate(&t);
        sceGumRotateY(g->B.yaw);
        render_char_light_begin(chest);
        chr_draw_any(&g->B.chr, g->B.skin);
        render_char_light_end();
        sceGumLoadIdentity();
    }
    sceGuDisable(GU_LIGHTING);
    render_shadow_blob(p->pos, 0.55f, floor_cb, g);
    for (int i = 0; i < g->n_en; i++)
        if (g->en[i].enabled && !g->en[i].dead) render_shadow_blob(g->en[i].pos, 0.5f, floor_cb, g);
    if (g->has_boss && g->boss_enabled && !g->boss_hidden && g->B.st != BS_INACTIVE && g->B.st != BS_DEAD)
        render_shadow_blob(g->B.pos, 2.2f, floor_cb, g);
    render_fx_draw();
    render_end_frame();
    sceGuDisable(GU_FOG);

    /* 2D: brightness (settings), world markers, HUD; src/menu owns the bonfire menu and the death screen */
    menu_draw_brightness(&M.set);
    HudFrame hf;
    hud_frame_clear(&hf);
    prg_fill_hud(&g->prg, &g->ui, &hf);
    hf.menu_on = 0;
    if (hf.banner == IMG_DIED) hf.banner = -1;
    hf.sp = g->pact.sp;
    hf.sp_max = g->pact.sp_max;
    if (!asy_has_estus(g)) hf.estus_max = -1;
    hf.boss_on = g->boss_bar && g->has_boss && g->B.hp > 0;
    if (g->has_boss) { hf.boss_hp = (float)g->B.hp; hf.boss_hp_max = (float)g->B.hp_max; }
    hf.boss_name = g->ui_ok ? ui_str(&g->ui, S_BOSS_ASYLUM) : "";
    hf.prompt = (prg_locked(&g->prg) || p->dead || menu_screen(&M)) ? NULL : g->has_ev ? ev_prompt(&g->ev) : g->prompt_text;
    if (g->toast_text && hf.toast < 0) {
        hf.toast_text = g->toast_text;
        float t = g->toast_t;
        hf.toast_a = t < 0.3f ? t / 0.3f : t > 2.4f ? (3.0f - t) / 0.6f : 1;
    }
    if (g->ev_fade > hf.fade) hf.fade = g->ev_fade;
    if (menu_screen(&M) == MENU_DIED) hf.hide_hud = 1;
    hud_tick(&g->hud, &hf, ASY_DT);
    ui_begin();
    if (g->has_ev)
        for (int i = 0; i < (int)g->ev.n_treas; i++) {
            float tp[3], sx, sy, dp;
            if (!ev_treasure_get(&g->ev, i, tp)) continue;
            tp[1] += 0.3f;
            if (asy_flat_dist(tp, g->cam.eye) < 25 && ui_project(&vp, tp, &sx, &sy, &dp))
                ui_glow(sx, sy, 6 + 2 * sinf(g->time * 4 + i), 0x80d0f0ff);
        }
    if (g->zone == ASY_ZONE_FIRELINK)
        for (int i = 0; i < FL.n_tr; i++) {
            float tp[3] = {FL.tr[i].pos[0], FL.tr[i].pos[1] + 0.3f, FL.tr[i].pos[2]}, sx, sy, dp;
            if (FL.tr[i].taken) continue;
            if (asy_flat_dist(tp, g->cam.eye) < 25 && ui_project(&vp, tp, &sx, &sy, &dp))
                ui_glow(sx, sy, 6 + 2 * sinf(g->time * 4 + i), 0x80d0f0ff);
        }
    for (int i = 0; i < ASY_MAX_BOMB; i++) {
        const Bomb *b = &g->bomb[i];
        if (!b->on) continue;
        float k = b->t / b->len, bp[3], sx, sy, dp;
        for (int a = 0; a < 3; a++) bp[a] = b->p0[a] + (b->p1[a] - b->p0[a]) * k;
        bp[1] += 3.0f * k * (1 - k) * 4;
        if (ui_project(&vp, bp, &sx, &sy, &dp)) ui_glow(sx, sy, 8, 0xc02080ff);
    }
    if (g->prg.stain_on) hud_draw_stain(&g->hud, &vp, g->prg.stain_pos);
    if (g->lock >= 0) {
        float sx, sy, dp;
        if (ui_project(&vp, g->lock_pt, &sx, &sy, &dp)) {
            ui_rect(sx - 3, sy - 3, 7, 7, 0xff202020);
            ui_rect(sx - 2, sy - 2, 5, 5, 0xffe8f4ff);
        }
    }
    if (!menu_blocks_game(&M)) hud_draw(&g->hud, &hf);
    if (g->zone == ASY_ZONE_FIRELINK && FL.say_t >= 0 && FL.say_cur < FL.n_say) ui_box_text(FL.say[FL.say_cur], 196);
    if (g_saving && g->ui_ok) ui_text_sh(&g->ui, UI_FONT_S, UI_W - 12, 12, ui_str(&g->ui, S_SAVING), 0xffc0d0e0, UI_RIGHT);
    if (g->end_demo) {   /* the crow: fade to black before the loading screen */
        float a = g->end_t / 1.5f;
        if (a > 1) a = 1;
        ui_rect(0, 0, UI_W, UI_H, ((u32)(a * 255) << 24));
    }
    ui_end();
    if (with_menu && menu_screen(&M)) menu_draw(&M);
    perf_end(PERF_LIST);
}

static int g_dl_max;   /* display list bytes, max over the stat second (g_list is 2 MB) */
static void present(void)
{
    diag_stage("gu finish");
    int dl = sceGuFinish();
    if (dl > g_dl_max) g_dl_max = dl;
    if (dl > (int)sizeof g_list - 65536) diag_event_sync("DISPLAY LIST NEAR OVERFLOW: %d B of %d", dl, (int)sizeof g_list);
    if (g_pipe && g_state == S_PLAY) { diag_stage("sim (GE busy)"); sim_steps(g_pipe_steps); }
    diag_stage("gu sync");
    diag_ge_wait("frame", 2000, 0);   /* GE hang -> logged + GE reset, the game goes on (src/diag) */
    if (g_shot_next[0]) {
        char nm[64];
        snprintf(nm, sizeof nm, "sh_%s.raw", g_shot_next);
        dump_raw(nm);
        printf("ash-ds: f%u shot %s (state %d zone %d screen %d)\n", (unsigned)g_frame, g_shot_next, g_state, G.zone,
               menu_screen(&M));
        g_shot_next[0] = 0;
    }
    if (g_dump_every > 0 && g_frame % g_dump_every == 0 && g_rec &&
        (g_rec_lo < 0 || ((int)g_frame >= g_rec_lo && (int)g_frame <= g_rec_hi))) {
        char nm[32];
        snprintf(nm, sizeof nm, "fr_%04d.raw", g_dumps++);
        dump_raw(nm);
    }
    diag_stage("vblank");
    sceDisplayWaitVblankStart();
    g_draw = sceGuSwapBuffers();
    g_frame++;
    diag_frame(g_frame);
    diag_stage("main loop");
}

/* full-screen loading screen (synchronous loads follow; one frame is enough to show it) */
static void loading_frame(const char *place)
{
    for (int k = 0; k < 2; k++) {
        sceGuStart(GU_DIRECT, g_list);
        sceGuClearColor(0xff000000);
        sceGuClearDepth(0);
        sceGuClear(GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT);
        if (G.ui_ok) {
            ui_begin();
            if (place) ui_text_sh(&G.ui, UI_FONT_L, UI_W / 2, 110, place, 0xffd0e0e8, UI_CENTER);
            ui_text_sh(&G.ui, UI_FONT_S, UI_W - 16, UI_H - 28, ui_str(&G.ui, S_LOADING), 0xffa0b0b8, UI_RIGHT);
            ui_end();
        }
        sceGuFinish();
        diag_ge_wait("loading frame", 2000, 0);
        if (k == 1 && place) { char nm[48]; snprintf(nm, sizeof nm, "loading_%d", G.zone); shot(nm); }
        if (g_shot_next[0]) { char nm[64]; snprintf(nm, sizeof nm, "sh_%s.raw", g_shot_next); dump_raw(nm);
                              printf("ash-ds: f%u shot %s\n", (unsigned)g_frame, g_shot_next); g_shot_next[0] = 0; }
        sceDisplayWaitVblankStart();
        g_draw = sceGuSwapBuffers();
        g_frame++;
        diag_frame(g_frame);
    }
}

/* ------------------------------------------------------------------ save (src/save) */

static void save_collect(void)
{
    SaveGame *s = &SG;
    sg_from_progress(s, &G.prg);
    s->level = (uint16_t)G.level;
    memcpy(s->attr, G.attr, 8);
    s->humanity = G.inv.humanity;
    s->area = G.zone == ASY_ZONE_FIRELINK ? 10 : 18;
    s->block = G.zone == ASY_ZONE_FIRELINK ? 2 : 1;
    s->bonfire = G.has_ev ? G.ev.respawn : 1021960;
    s->play_s = (uint32_t)G.time;
    if (G.prg.stain_on) { s->stain_area = s->area; s->stain_block = s->block; }
    if (G.has_ev) {
        SgZoneEvents *z = sg_zone_put(s, s->area, s->block);
        if (z) z->size = ev_save_write(&G.ev, z->data, SG_EVT_BYTES);
    }
    s->itm_size = G.has_items ? itm_save_write(&G.inv, s->itm_blob, SG_ITM_BYTES) : 0;
    if (s->itm_size < 0) s->itm_size = 0;
    sg_from_settings(s, &M.set);
}

static void save_game(Game *g, const char *why)
{
    (void)g;
    if (!g_in_game) return;
    if (sv_busy(&ST)) { g_save_dirty = 1; return; }
    save_collect();
    int r = sv_write_begin(&ST, &SG);
    g_saving = r == SV_BUSY;
    diag_event("save begin (%s) -> %d, zone %d.%d", why, r, SG.area, SG.block);
    printf("ash-ds: f%u autosave (%s) begin -> %d: zone %d.%d bonfire %u souls %d inv %d B\n", (unsigned)g_frame, why, r,
           SG.area, SG.block, (unsigned)SG.bonfire, (int)SG.souls, SG.itm_size);
}

static void save_poll(void)
{
    if (!g_saving) return;
    diag_stage_t("save poll", 5000);
    int r = sv_poll(&ST);
    diag_stage("main loop");
    if (r == SV_BUSY) return;
    g_saving = 0;
    diag_event("save end -> %s via %s (%d polls, last err %x)", r == SV_OK ? "OK" : "ERR", sv_src_name(ST.last_src),
               ST.frames, (unsigned)ST.last_err);
    g_has_save = g_has_save || r == SV_OK;
    printf("ash-ds: f%u autosave %s -> %s seq %u (%d polls)\n", (unsigned)g_frame, r == SV_OK ? "OK" : "ERR",
           sv_src_name(ST.last_src), (unsigned)SG.seq, ST.frames);
    if (g_save_dirty) { g_save_dirty = 0; save_game(&G, "dirty"); }
}

static void save_sync(const char *why)
{
    diag_stage_t("save sync", 45000);
    diag_event("save sync (%s)", why);
    while (sv_busy(&ST)) { sv_poll(&ST); sceDisplayWaitVblankStart(); }
    g_saving = 0;
    save_collect();
    int r = sv_write_sync(&ST, &SG);
    g_has_save = g_has_save || r == SV_OK;
    printf("ash-ds: save (%s) sync -> %s seq %u\n", why, r == SV_OK ? "OK" : "ERR", (unsigned)SG.seq);
    diag_event("save sync -> %s (last err %x)", r == SV_OK ? "OK" : "ERR", (unsigned)ST.last_err);
    diag_stage("main loop");
}

/* ------------------------------------------------------------------ menus over the game */

static void view_fill(void)
{
    menu_fill_view_from_char(&M, &V);
    V.level = G.level;
    memcpy(V.attr, G.attr, 8);
    V.souls = (int)G.prg.souls;
    V.humanity = G.inv.humanity;
    if (G.has_items && G.class_row) {
        itm_calc(&G.inv, &G.idb, G.attr, &G.ist);
        itm_menu_fill(&G.idb, &G.inv, &G.ist, &V, &IM);
    }
    V.hp = G.pact.hp;
    V.hp_max = G.pact.hp_max;
    V.sp = G.pact.sp;
    V.sp_max = G.pact.sp_max;
    menu_set_view(&M, &V);
}

static void reset_progress(void)
{
    prg_init(&G.prg, 0);
    prg_bind_hp(&G.prg, &G.pact.hp, &G.pact.hp_max);
    if (G.has_prg)
        for (int a = 1; a < PRG_ANIM_COUNT; a++) {
            int c = anim_find(&G.kprg, prg_anim_name[a]);
            if (c >= 0) prg_set_clip_len(&G.prg, a, anim_duration(&G.kprg, c));
        }
}

static void unload_zone(void)
{
    if (!g_in_game) return;
    gpu_zone_free();
    game_zone_unload(&G);
    G.zone_step = NULL;
    g_in_game = 0;
    struct mallinfo mi = mallinfo();
    printf("ash-ds: zone unloaded, heap in use %d KB\n", mi.uordblks / 1024);
    diag_event("zone unloaded, heap %u KB", (unsigned)(mi.uordblks / 1024));
}

static void body_for_class(void)
{
    char nm[32];
    snprintf(nm, sizeof nm, "cls_%d_%c.ashc", M.chr.class_row, M.chr.gender ? 'f' : 'm');
    game_set_body(&G, load, nm);
}

static int enter_asylum(int scenario)
{
    diag_event_sync("zone load: Asylum m18_01 (heap %u KB)", (unsigned)(diag_heap_used() / 1024));
    loading_frame(ui_str(&G.ui, S_AREA_ASYLUM));
    if (game_zone_load_asylum(&G, load) < 0) { printf("ash-ds: Asylum load failed\n"); return -1; }
    game_start(&G, scenario);
    g_in_game = 1;
    gpu_zone_setup();
    struct mallinfo mi = mallinfo();
    printf("ash-ds: Asylum loaded, heap in use %d KB\n", mi.uordblks / 1024);
    diag_event_sync("zone loaded: Asylum, heap %u KB (peak %u KB), max free block %u KB", (unsigned)(diag_heap_used() / 1024),
                    (unsigned)(diag_heap_peak() / 1024), (unsigned)(sceKernelMaxFreeMemSize() / 1024));
    return 0;
}

static int enter_firelink(void)
{
    diag_event_sync("zone load: Firelink m10_02 (heap %u KB)", (unsigned)(diag_heap_used() / 1024));
    loading_frame(FL.area ? FL.area : "");
    unload_zone();
    fl_load_once();
    loading_frame(FL.area);
    float pos[4] = {47.41f, -59.82f, 53.30f, -2.53f};   /* fallback: region 1022100 START [проверено: ents.json] */
    if (game_zone_load_plain(&G, load, "zone_m10_02.pak", ASY_ZONE_FIRELINK, pos) < 0) return -1;
    game_start(&G, ASY_SC_NONE);   /* player, camera, input state for the new zone (no events: nothing else runs) */
    G.inp.scheme = M.set.scheme;
    int r = world_find_region(&G.W, WR_RESPAWN, 0);
    if (r >= 0) {
        memcpy(G.respawn, G.W.reg[r].pos, 12);
        G.respawn[3] = G.W.reg[r].yaw;
        game_place_player(&G, G.respawn, G.respawn[3]);
    }
    fl_setup_zone(&G);
    G.autopilot = 0;
    g_in_game = 1;
    gpu_zone_setup();
    G.prg.banner = -1;   /* the Asylum's area title / victory banner does not follow into the new zone */
    G.prg.banner_sticky = 0;
    fl_toast(&G, FL.area);
    struct mallinfo mi = mallinfo();
    printf("ash-ds: Firelink Shrine loaded at (%.2f %.2f %.2f), heap in use %d KB\n", G.pl.pos[0], G.pl.pos[1],
           G.pl.pos[2], mi.uordblks / 1024);
    diag_event_sync("zone loaded: Firelink, heap %u KB (peak %u KB)", (unsigned)(diag_heap_used() / 1024),
                    (unsigned)(diag_heap_peak() / 1024));
    return 0;
}

static void new_game(int scenario)
{
    loading_frame(NULL);
    unload_zone();
    reset_progress();
    body_for_class();
    sg_init(&SG);
    sg_from_menu_char(&SG, &M.chr);
    sg_from_settings(&SG, &M.set);
    G.inp.scheme = M.set.scheme;
    int gift = M.chr.gift_idx ? 2400 + M.chr.gift_idx : 0;
    G.save_fn = save_game;
    if (enter_asylum(scenario) < 0) return;
    game_set_class(&G, M.chr.class_row, gift, M.chr.attr, M.chr.level);
    G.inv.humanity = M.chr.humanity;
    G.inp.scheme = M.set.scheme;
    diag_event_sync("NEW GAME class %d gender %d", M.chr.class_row, M.chr.gender);
    printf("ash-ds: NEW GAME \"%s\" class %d gender %d gift %d level %d hp %.0f\n", M.chr.name, M.chr.class_row,
           M.chr.gender, gift, M.chr.level, G.pact.hp_max);
    g_state = S_PLAY;
}

static void continue_game(void)
{
    loading_frame(NULL);
    unload_zone();
    reset_progress();
    MenuSettings set = M.set;
    sg_to_menu_char(&SG, &M.chr);
    sg_to_settings(&SG, &set);
    M.set = set;
    body_for_class();
    G.save_fn = save_game;
    G.class_row = SG.class_row;
    G.level = SG.level;
    memcpy(G.attr, SG.attr, 8);
    if (SG.area == 10 && SG.block == 2) {
        if (enter_firelink() < 0) return;
    } else {
        if (enter_asylum(ASY_SC_NONE) < 0) return;
        const SgZoneEvents *z = sg_zone_get(&SG, 18, 1);
        int r = z && z->size > 0 ? ev_save_read(&G.ev, z->data, z->size) : -99;
        float pos[3], yaw;
        if (ev_respawn(&G.ev, pos, &yaw)) game_place_player(&G, pos, yaw);
        printf("ash-ds: Asylum events restored %d\n", r);
    }
    if (G.has_items) {
        int r = SG.itm_size > 0 ? itm_save_read(&G.inv, &G.idb, SG.itm_blob, SG.itm_size) : -99;
        G.inv.estus = &G.prg.estus;
        printf("ash-ds: inventory restored %d (%d entries)\n", r, G.inv.n);
    }
    sg_to_progress(&SG, &G.prg);
    G.inv.humanity = SG.humanity;
    game_apply_equipment(&G);
    G.pact.hp = SG.hp > 0 ? SG.hp : G.pact.hp_max;
    if (G.pact.hp > G.pact.hp_max) G.pact.hp = G.pact.hp_max;
    G.inp.scheme = M.set.scheme;
    menu_close(&M);
    diag_event_sync("CONTINUE zone %d.%d", SG.area, SG.block);
    printf("ash-ds: CONTINUE \"%s\" class %d level %d zone %s bonfire %u souls %d estus %d hp %.0f/%.0f\n", SG.name,
           SG.class_row, SG.level, sg_zone_name(SG.area, SG.block), (unsigned)SG.bonfire, (int)G.prg.souls,
           (int)G.prg.estus, G.pact.hp, G.pact.hp_max);
    g_state = S_PLAY;
}

static void to_title(void)
{
    save_sync("to title");
    loading_frame(NULL);
    unload_zone();
    menu_set_has_save(&M, g_has_save);
    menu_open(&M, MENU_TITLE);
    g_state = S_TITLE;
}

static int g_bf_open, g_died_open;
static int g_crash_at = -1, g_crash_kind;   /* demo.txt crash=...@frame */

static void menu_event(int ev)
{
    switch (ev) {
    case MEV_NEW_GAME: break;   /* handled by the caller (scenario) */
    case MEV_QUIT: g_running = 0; break;
    case MEV_SETTINGS: G.inp.scheme = M.set.scheme; break;
    case MEV_REST:   /* progress already refilled at the bonfire (event / fl_step); here: the autosave DS1 does */
        save_game(&G, "bonfire menu rest");
        break;
    case MEV_LEVELUP:
        G.level = V.level;
        memcpy(G.attr, V.attr, 8);
        G.prg.souls = V.souls;
        game_apply_equipment(&G);
        view_fill();
        printf("ash-ds: LEVEL UP -> %d, souls %d, hp max %.0f\n", G.level, (int)G.prg.souls, G.pact.hp_max);
        break;
    case MEV_USE_ITEM: {
        int k = M.ev_arg;
        if (k >= 0 && k < IM.n) game_use_item(&G, IM.inv_idx[k]);
        view_fill();
        break;
    }
    case MEV_LEAVE_BONFIRE: G.ext_leave_bonfire = 1; break;
    case MEV_TO_TITLE: to_title(); break;
    default: break;
    }
}

/* ------------------------------------------------------------------ autopilot (demo.txt) */

enum { SC_NONE, SC_FULL, SC_HOLLOW, SC_BOSS, SC_DEATH, SC_CONTINUE, SC_FIRELINK, SC_FLRUN, SC_PROBE, SC_VHOLLOW, SC_N };
static const char *const sc_name[SC_N] = {"none", "full", "hollow", "boss", "death", "continue", "firelink", "flrun", "probe", "vhollow"};

enum { O_WAIT, O_PRESS, O_SHOT, O_NEWGAME, O_ASYLUM, O_UNTIL_FL, O_GOTO, O_TALK, O_USE, O_REST, O_PAUSE, O_MPRESS, O_REC,
       O_ROLL, O_TURN, O_LTAP, O_END };
typedef struct { int op; u32 a; float x, z; int n; const char *s; } Step;
#define W_(n) {O_WAIT, 0, 0, 0, n, 0}
#define P_(b, n) {O_PRESS, b, 0, 0, n, 0}
#define SH(s) {O_SHOT, 0, 0, 0, 0, s}
#define MP_(b, n) {O_MPRESS, b, 0, 0, n, 0}   /* menu only: the game never sees it (no stray Estus on O) */
#define GO(x, z, s) {O_GOTO, 0, x, z, 0, s}
#define RUN(x, z, s) {O_GOTO, 1, x, z, 0, s}   /* X held: run */
#define X_ PSP_CTRL_CROSS
#define O__ PSP_CTRL_CIRCLE
#define U_ PSP_CTRL_UP
#define D_ PSP_CTRL_DOWN
#define R_ PSP_CTRL_RIGHT
#define L_ PSP_CTRL_LEFT
#define ST_ PSP_CTRL_START

/* title -> main -> "Новая игра" -> creation: Класс row, list opens on Рыцарь (class_idx 1 by default), confirm,
   down to "Готово", "Начать игру?" -> Да */
#define NEWGAME_KNIGHT                                                                                              \
    W_(50), SH("01_title"), P_(X_, 30), SH("02_main"), {O_NEWGAME, 0, 0, 0, 0, 0}, P_(X_, 40), SH("03_create"),      \
    P_(D_, 4), P_(D_, 6), P_(X_, 12), SH("04_class_list"), P_(X_, 30), P_(D_, 4), P_(D_, 4), P_(D_, 4), P_(X_, 14),  \
    SH("05_create_confirm"), P_(X_, 4)

static const Step SC_FULL_S[] = {
    NEWGAME_KNIGHT, {O_ASYLUM, ASY_SC_ROUTE, 0, 0, 0, 0}, {O_UNTIL_FL, 0, 0, 0, 0, 0}, W_(40), SH("30_firelink_arrive"),
    GO(51.2f, 61.3f, "to the Crestfallen Warrior"), {O_TALK, 0, 0, 0, 0, 0}, W_(20), SH("31_crestfallen_talk"),
    W_(100), {O_TALK, 0, 0, 0, 0, 0}, W_(40), SH("32_crestfallen_talk2"), W_(100),
    GO(46.0f, 54.0f, "back past the bonfire"), GO(40.8f, 53.6f, "to the corpse o0500_0000"), {O_USE, 0, 0, 0, 0, 0},
    W_(15), SH("33_loot"), W_(60),
    GO(50.5f, 55.0f, "to the bonfire"), {O_USE, 0, 0, 0, 0, 0}, W_(40), SH("34_bonfire_lit"), W_(60),
    /* sitting down refills (fl_step), the menu's "Отдохнуть у костра" saves; 36 is taken after leaving the menu so
       the HUD (hidden under the menu) shows the refill against 34 (the Asylum leaves the flask empty, HP short) */
    {O_REST, 0, 0, 0, 0, 0}, W_(10), SH("35_bonfire_menu"), P_(X_, 30), MP_(O__, 45), SH("36_rested"), W_(30),
    {O_PAUSE, 0, 0, 0, 0, 0}, P_(ST_, 20), SH("37_pause"), P_(R_, 8), P_(X_, 20), SH("38_inventory"), P_(O__, 12),
    P_(L_, 8), P_(X_, 20), SH("39_equipment"), P_(O__, 12), P_(R_, 8), P_(R_, 8), P_(X_, 20), SH("40_status"),
    P_(O__, 12), P_(O__, 20), W_(30), {O_END, 0, 0, 0, 0, 0},
};
/* video: the title and the creation with the class list paged through (back to Рыцарь), then the hollow duel */
#define NEWGAME_SHOW                                                                                                \
    W_(75), SH("01_title"), P_(X_, 30), SH("02_main"), {O_NEWGAME, 0, 0, 0, 0, 0}, P_(X_, 40), SH("03_create"),      \
    P_(D_, 10), P_(D_, 10), P_(X_, 20), SH("04_class_list"), P_(D_, 22), P_(D_, 22), P_(D_, 22), P_(D_, 22),          \
    P_(D_, 22), P_(U_, 14), P_(U_, 14), P_(U_, 14), P_(U_, 14), P_(U_, 30), P_(X_, 30), P_(D_, 8), P_(D_, 8),         \
    P_(D_, 8), P_(X_, 30), SH("05_create_confirm"), P_(X_, 4)
static const Step SC_VHOLLOW_S[] = {NEWGAME_SHOW, {O_ASYLUM, ASY_SC_HOLLOW, 0, 0, 0, 0}, {O_END, 0, 0, 0, 0, 0}};
static const Step SC_HOLLOW_S[] = {NEWGAME_KNIGHT, {O_ASYLUM, ASY_SC_HOLLOW, 0, 0, 0, 0}, {O_END, 0, 0, 0, 0, 0}};
static const Step SC_BOSS_S[] = {NEWGAME_KNIGHT, {O_ASYLUM, ASY_SC_BOSS, 0, 0, 0, 0}, {O_END, 0, 0, 0, 0, 0}};
static const Step SC_DEATH_S[] = {NEWGAME_KNIGHT, {O_ASYLUM, ASY_SC_DEATH, 0, 0, 0, 0}, {O_END, 0, 0, 0, 0, 0}};
/* run 2 after "full": the save of the Firelink bonfire -> "Продолжить" */
static const Step SC_CONTINUE_S[] = {
    W_(50), SH("50_title"), P_(X_, 30), SH("51_main_continue"), P_(X_, 60), SH("52_continued"), W_(60),
    {O_PAUSE, 0, 0, 0, 0, 0}, P_(ST_, 20), P_(R_, 8), P_(R_, 8), P_(X_, 20), SH("53_status"), P_(O__, 12),
    P_(L_, 8), P_(X_, 20), SH("54_inventory"), P_(O__, 12), P_(O__, 20), W_(20),
    /* the ○ that closed the pause must not drink (hp is full); a fresh ○ in the game does: "Estus: drink" exactly once */
    SH("55_menu_closed"), P_(O__, 50), SH("56_estus_drink"), W_(20), {O_END, 0, 0, 0, 0, 0},
};
/* dev: straight to Firelink after the creation (skips the Asylum) */
static const Step SC_FIRELINK_S[] = {
    NEWGAME_KNIGHT, {O_UNTIL_FL, 1, 0, 0, 0, 0}, W_(40), SH("30_firelink_arrive"),
    GO(51.2f, 61.3f, "to the Crestfallen Warrior"), {O_TALK, 0, 0, 0, 0, 0}, W_(20), SH("31_crestfallen_talk"),
    W_(100), {O_TALK, 0, 0, 0, 0, 0}, W_(40), SH("32_crestfallen_talk2"), W_(40), GO(46.0f, 54.0f, "back past the bonfire"), GO(40.8f, 53.6f, "to the corpse o0500_0000"),
    {O_USE, 0, 0, 0, 0, 0}, W_(15), SH("33_loot"), W_(60), GO(50.5f, 55.0f, "to the bonfire"),
    {O_USE, 0, 0, 0, 0, 0}, W_(40), SH("34_bonfire_lit"), W_(60), {O_REST, 0, 0, 0, 0, 0}, W_(10),
    SH("35_bonfire_menu"), P_(X_, 30), MP_(O__, 45), SH("36_rested"), W_(30), {O_PAUSE, 0, 0, 0, 0, 0}, P_(ST_, 20), SH("37_pause"), P_(R_, 8), P_(X_, 20), SH("38_inventory"),
    P_(O__, 12), P_(O__, 20), W_(20), {O_END, 0, 0, 0, 0, 0},
};

/* video: a run round Firelink Shrine (dev start like "firelink"; fr_ dumps only from the arrival, O_REC) */
static const Step SC_FLRUN_S[] = {
    {O_REC, 0, 0, 0, 0, 0}, NEWGAME_KNIGHT, {O_UNTIL_FL, 1, 0, 0, 0, 0}, {O_REC, 1, 0, 0, 0, 0}, W_(45),
    {O_LTAP, 0, 0, 0, 10, 0}, SH("60_flrun_start"),   /* every leg below reached without a warp (probe runs) */
    RUN(51.0f, 51.5f, "round the bonfire"), RUN(55.0f, 53.0f, ""), RUN(57.5f, 58.0f, "past the tree"),
    RUN(55.0f, 61.5f, ""), RUN(52.0f, 63.0f, ""), RUN(48.0f, 62.0f, ""), RUN(45.5f, 58.0f, ""),
    RUN(38.0f, 55.0f, "to the ruins"), SH("61_flrun_ruins"), RUN(42.0f, 60.0f, ""), RUN(38.0f, 64.0f, "out of the bowl"),
    RUN(31.0f, 65.0f, "stairs"), RUN(34.0f, 72.0f, "up the stairs"), SH("62_flrun_stairs"), W_(15),
    RUN(31.0f, 65.0f, "back down"), RUN(38.0f, 64.0f, ""), {O_ROLL, 0, 42.0f, 60.0f, 0, "roll"}, SH("63_flrun_roll"),
    {O_ROLL, 0, 45.5f, 61.0f, 0, "roll"}, W_(10), GO(48.6f, 60.4f, "to the warrior"),
    {O_TURN, 0, 51.2f, 61.3f, 20, "face the warrior"}, {O_LTAP, 0, 0, 0, 45, 0}, SH("64_flrun_warrior"), W_(30),
    RUN(48.0f, 55.0f, "back to the bonfire"), GO(50.2f, 55.2f, "bonfire"), {O_TURN, 0, 51.8f, 56.5f, 12, 0},
    {O_LTAP, 0, 0, 0, 45, 0}, SH("65_flrun_bonfire"), W_(20), {O_END, 0, 0, 0, 0, 0},
};

/* dev: route probe - like "flrun" up to the arrival, then RUN to each "x z" of route.txt (stage dir), logged */
static Step SC_PROBE_S[64] = {NEWGAME_KNIGHT, {O_UNTIL_FL, 1, 0, 0, 0, 0}, W_(30)};

typedef struct {
    int sc;
    const Step *s;
    int i, t, warps;
    float best, stuck_t;
    int done;
    u32 menu_buttons;           /* buttons for the menu this frame */
    RawPad pad;                 /* pad for the game this frame (when asy_auto does not drive) */
    int bf_step;                /* bonfire menu under the Asylum autopilot: 0 none, frames in it */
} Bot;
static Bot B;
static int B_active(void) { return B.s != NULL; }

static void bot_stick_to(float wx, float wz, int run)
{
    float l = sqrtf(wx * wx + wz * wz);
    if (l < 1e-4f) return;
    wx /= l; wz /= l;
    float by = G.pl.basis_valid ? G.pl.basis_yaw : G.cam.yaw, s = sinf(by), c = cosf(by);
    float fx = -s * wx - c * wz, rx = c * wx - s * wz;   /* inverse of stick_world() (asy_game.c) */
    B.pad.lx = (u8)(128 + 120 * rx);
    B.pad.ly = (u8)(128 - 120 * fx);
    if (run) B.pad.buttons |= PSP_CTRL_CROSS;
}

static void bot_next(void) { B.i++; B.t = 0; B.best = 1e9f; B.stuck_t = 0; }

static void bot_tick(void)
{
    B.menu_buttons = 0;
    B.pad = (RawPad){0, 128, 128};
    if (B.done || !B.s) return;
    /* menus opened by the game while the Asylum autopilot drives: bonfire -> rest (autosave), leave; */
    if (G.autopilot && g_state == S_PLAY) {
        if (menu_screen(&M) == MENU_BONFIRE) {
            B.bf_step++;
            if (B.bf_step == 12) shot("10_bonfire_menu_asylum");
            if (B.bf_step == 14) B.menu_buttons = PSP_CTRL_CROSS;            /* Отдохнуть у костра */
            if (B.bf_step == 40) B.menu_buttons = PSP_CTRL_CIRCLE;           /* leave */
        } else B.bf_step = 0;
    }
    const Step *st = &B.s[B.i];
    switch (st->op) {
    case O_WAIT: if (++B.t >= st->n) bot_next(); break;
    case O_PRESS:
        if (B.t < 2) { B.menu_buttons = st->a; B.pad.buttons |= st->a; }
        if (++B.t >= 2 + st->n) bot_next();
        break;
    case O_MPRESS:
        if (B.t < 2) B.menu_buttons = st->a;
        if (++B.t >= 2 + st->n) bot_next();
        break;
    case O_SHOT: shot(st->s); bot_next(); break;
    case O_NEWGAME:   /* a save exists (run after "full"): cursor starts on "Продолжить" -> one down */
        if (B.t == 0 && g_has_save) B.menu_buttons = PSP_CTRL_DOWN;
        if (++B.t >= 6) bot_next();
        break;
    case O_ASYLUM:   /* the game started with this asy_auto scenario (MEV_NEW_GAME) - wait until it is over */
        if (g_state == S_PLAY && G.autopilot && (G.au.done || G.end_demo)) bot_next();
        if (g_state == S_PLAY && !G.autopilot) bot_next();
        break;
    case O_UNTIL_FL:
        if (st->a == 1 && g_state == S_PLAY && G.zone == ASY_ZONE_ASYLUM && !G.end_demo) {
            G.end_demo = 1;   /* dev scenario: the crow right away */
            G.end_t = 1.5f;
        }
        if (g_state == S_PLAY && G.zone == ASY_ZONE_FIRELINK) bot_next();
        break;
    case O_GOTO: {
        float to[3] = {st->x, G.pl.pos[1], st->z};
        float d = asy_flat_dist(G.pl.pos, to);
        if (B.t == 0) printf("ash-ds: bot goto (%.1f %.1f) %s, %.1f m\n", st->x, st->z, st->s ? st->s : "", d);
        B.t++;
        if (d < 1.0f) { printf("ash-ds: bot reached %s in %d frames\n", st->s ? st->s : "", B.t); bot_next(); break; }
        if (d < B.best - 0.2f) { B.best = d; B.stuck_t = 0; } else B.stuck_t += ASY_DT;
        if (B.stuck_t > 6.0f || B.t > 30 * 30) {   /* honest warp: logged and counted */
            float pos[3] = {st->x, G.pl.pos[1] + 1.0f, st->z};
            game_place_player(&G, pos, G.pl.yaw);
            B.warps++;
            printf("ash-ds: bot STUCK at (%.1f %.1f %.1f) best %.1f m -> warp to %s (%d)\n", G.pl.pos[0], G.pl.pos[1],
                   G.pl.pos[2], B.best, st->s ? st->s : "", B.warps);
            bot_next();
            break;
        }
        if (!prg_locked(&G.prg)) bot_stick_to(st->x - G.pl.pos[0], st->z - G.pl.pos[2], st->a == 1);
        break;
    }
    case O_REC: g_rec = (int)st->a; bot_next(); break;
    case O_ROLL:   /* release X (stop running), a short X tap with the stick toward (x, z) = dodge, then the roll */
        bot_stick_to(st->x - G.pl.pos[0], st->z - G.pl.pos[2], 0);
        if (B.t >= 4 && B.t < 7) B.pad.buttons |= PSP_CTRL_CROSS;
        if (B.t == 4) printf("ash-ds: bot %s at (%.1f %.1f)\n", st->s ? st->s : "roll", G.pl.pos[0], G.pl.pos[2]);
        if (++B.t >= 30) bot_next();
        break;
    case O_TURN: {   /* a light stick push toward (x, z): the knight turns (a step at most) */
        float wx = st->x - G.pl.pos[0], wz = st->z - G.pl.pos[2];
        bot_stick_to(wx, wz, 0);
        B.pad.lx = (u8)(128 + (B.pad.lx - 128) * 60 / 120);
        B.pad.ly = (u8)(128 + (B.pad.ly - 128) * 60 / 120);
        if (++B.t >= st->n) bot_next();
        break;
    }
    case O_LTAP:   /* L tap: camera behind the knight (no lock target in Firelink) */
        if (B.t < 2) B.pad.buttons |= PSP_CTRL_LTRIGGER;
        if (++B.t >= 2 + st->n) bot_next();
        break;
    case O_TALK: case O_USE:
        if (B.t == 2) B.pad.buttons |= PSP_CTRL_UP;   /* scheme A: d-pad up = action */
        if (++B.t >= 4) bot_next();
        break;
    case O_REST:   /* act at the bonfire, wait until src/menu's bonfire screen is up */
        if (B.t == 2) B.pad.buttons |= PSP_CTRL_UP;
        B.t++;
        if (menu_screen(&M) == MENU_BONFIRE) bot_next();
        else if (B.t > 300) { printf("ash-ds: bot: no bonfire menu\n"); bot_next(); }
        break;
    case O_PAUSE: view_fill(); bot_next(); break;
    case O_END: B.done = 1; printf("ash-ds: bot script done (%s), warps %d\n", sc_name[B.sc], B.warps); break;
    }
}

/* ------------------------------------------------------------------ one fixed step */

/* Buttons that were down while a menu held the game (or on the frame it closed): the game does not see them until
   they are released. Else the ○ that closes the pause menu reaches the game as a fresh press on the same or the next
   frame and drinks Estus (○ = Estus, the controls design notes). Armed in the main loop right after menu_tick. */
static u32 g_swallow;

static RawPad game_pad(void)
{
    RawPad raw = {0, 128, 128};
    if (B.s) raw = B.pad;
    else {
        SceCtrlData pad;
        sceCtrlPeekBufferPositive(&pad, 1);
        raw.buttons = pad.Buttons; raw.lx = pad.Lx; raw.ly = pad.Ly;
    }
    return raw;
}

static void sim_steps(int steps)
{
    perf_begin(PERF_SIM);
    for (int s = 0; s < steps; s++) {
        RawPad raw = game_pad();
        if (menu_blocks_game(&M)) { g_swallow = raw.buttons; raw.buttons = 0; raw.lx = raw.ly = 128; }
        else { g_swallow &= raw.buttons; raw.buttons &= ~g_swallow; }
        game_step(&G, &raw);
        for (int k = 0; k < G.n_fx; k++) {
            if (G.fx_kind[k] == 3) fx_roll_dust(G.fx_pos[k], G.pl.yaw);
            else fx_hit_spark(G.fx_pos[k], G.fx_dir[k], G.fx_kind[k] == 2);
        }
    }
    perf_end(PERF_SIM);
}

/* src/diag stat line (main thread, once a second): where the player is and what the game does */
static int diag_stat(char *b, int n)
{
    int dl = g_dl_max;
    g_dl_max = 0;
    if (g_state != S_PLAY || !g_in_game)
        return snprintf(b, n, "title menu=%d dl=%dKB ge_resets=%u", menu_screen(&M), dl / 1024, (unsigned)diag_ge_resets());
    return snprintf(b, n, "z%d st=%s pos=%.1f,%.1f,%.1f hp=%.0f en=%d boss=%d/%d menu=%d save=%d dl=%dKB ge_resets=%u",
                    G.zone, asy_pst_name[G.pl.st], G.pl.pos[0], G.pl.pos[1], G.pl.pos[2], G.pact.hp, G.n_en,
                    G.has_boss ? (int)G.B.st : -1, G.has_boss ? (int)G.B.hp : 0, menu_screen(&M), g_saving, dl / 1024,
                    (unsigned)diag_ge_resets());
}

static void gu_setup(void *fbp0, void *fbp1, void *zbp)
{
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
    ScePspFVector3 ldir = {-0.35f, 0.8f, -0.45f};
    sceGuLight(0, GU_DIRECTIONAL, GU_DIFFUSE, &ldir);
    sceGuLightColor(0, GU_DIFFUSE, 0xff86b2d9);
    sceGuLightMode(GU_SINGLE_COLOR);
    sceGuAmbient(0xff4a4a4a);
    sceGuEnable(GU_LIGHT0);
    sceGuFinish();
    sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);
    sceDisplayWaitVblankStart();
    sceGuDisplay(GU_TRUE);
}

int main(int argc, char *argv[])
{
    int th = sceKernelCreateThread("cb_thread", cb_thread, 0x11, 0xFA0, 0, 0);
    if (th >= 0) sceKernelStartThread(th, 0, 0);
    /* hardware diagnostics first: log.txt / crash.txt / hang watchdog next to the EBOOT (src/diag, the hw-day1 design notes) */
    diag_init(argc > 0 ? argv[0] : NULL, ASH_DIAG_SHA);
    diag_set_exit_flag(&g_running);
    diag_set_stat_fn(diag_stat);
    scePowerSetClockFrequency(333, 333, 166);   /* CPU 333 / bus 166 MHz (docs/psp-cost-model.md) */
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);
    printf("ash-ds: boot, free=%u KB max block=%u KB, cpu %d MHz bus %d MHz\n",
           (unsigned)(sceKernelTotalFreeMemSize() / 1024), (unsigned)(sceKernelMaxFreeMemSize() / 1024),
           scePowerGetCpuClockFrequency(), scePowerGetBusClockFrequency());

    /* demo.txt: autopilot */
    int sc = SC_NONE;
    {
        u8 *buf = NULL;
        if (ash_load_file("demo.txt", &buf, NULL, 0) > 0) {
            char nm[32] = "", opt[32] = "";
            sscanf((const char *)buf, "%d %d %31s %31s", &g_dump_every, &g_max_frames, nm, opt);
            for (int i = 1; i < SC_N; i++) if (!strcmp(nm, sc_name[i])) sc = i;
            if (!strcmp(opt, "slow")) { g_fast = 0; g_pipe = 0; }
            if (!strcmp(opt, "nopipe")) g_pipe = 0;
            if (!strncmp(opt, "rec=", 4)) sscanf(opt + 4, "%d-%d", &g_rec_lo, &g_rec_hi);
            /* crash=<hw|sim|hang>@<frame>: diag self-test (src/diag) at that frame */
            if (!strncmp(opt, "crash=", 6)) {
                char k[8] = "";
                sscanf(opt + 6, "%7[a-z]@%d", k, &g_crash_at);
                g_crash_kind = !strcmp(k, "sim") ? 1 : !strcmp(k, "hang") ? 2 : 0;
            }
            free(buf);
        }
    }
    static const Step *const scripts[SC_N] = {NULL, SC_FULL_S, SC_HOLLOW_S, SC_BOSS_S, SC_DEATH_S, SC_CONTINUE_S,
                                              SC_FIRELINK_S, SC_FLRUN_S, SC_PROBE_S, SC_VHOLLOW_S};
    if (sc == SC_PROBE) {
        u8 *rb = NULL;
        int k = 0;
        while (SC_PROBE_S[k].op != O_WAIT || SC_PROBE_S[k].n != 30) k++;
        k++;
        if (ash_load_file("route.txt", &rb, NULL, 0) > 0) {
            char *q = (char *)rb;
            float x, z;
            int run, used;
            while (k < 62 && sscanf(q, "%f %f %d%n", &x, &z, &run, &used) == 3) {
                SC_PROBE_S[k++] = (Step){O_GOTO, (u32)run, x, z, 0, "probe"};
                q += used;
            }
        }
        SC_PROBE_S[k] = (Step){O_END, 0, 0, 0, 0, 0};
    }
    memset(&B, 0, sizeof B);
    B.sc = sc;
    B.s = scripts[sc];
    B.best = 1e9f;

    void *fbp0 = guGetStaticVramBuffer(BUF_WIDTH, SCR_HEIGHT, GU_PSM_5650);
    void *fbp1 = guGetStaticVramBuffer(BUF_WIDTH, SCR_HEIGHT, GU_PSM_5650);
    void *zbp = guGetStaticVramBuffer(BUF_WIDTH, SCR_HEIGHT, GU_PSM_4444);
    int used = 3 * BUF_WIDTH * SCR_HEIGHT * 2;
    g_vram = (u8 *)sceGeEdramGetAddr() + used;
    g_vram_bytes = sceGeEdramGetSize() - used;
    gu_setup(fbp0, fbp1, zbp);
    g_draw = fbp0;
    pspDebugScreenInitEx((void *)(0x44000000 | (u32)fbp0), PSP_DISPLAY_PIXEL_FORMAT_565, 0);
    pspDebugScreenEnableBackColor(0);
    render_init();
    g_render.flags = RF_PSP_DEFAULT;

    /* common data (combat, player clips, items, HUD) + menus + the save on the memory stick */
    if (game_load_common(&G, load) < 0) { printf("ash-ds: load failed\n"); sceKernelExitGame(); return 0; }
    loading_frame(NULL);
    float vd = anim_selftest_vfpu();
    printf("ash-ds: VFPU selftest max diff %.2g -> %s\n", vd, anim_vfpu() ? "VFPU" : "C fallback");
    if (menu_init(&M) != 0) { printf("ash-ds: menu files missing\n"); sceKernelExitGame(); return 0; }
    {
        u8 *f;
        int n = load("items.ashui", &f);
        g_icons_ok = n > 0 && ui_load(&g_icons, f, n) == 0;
        if (g_icons_ok) menu_set_ext_images(&M, &g_icons);
    }
    {
        float ld[3] = {-0.35f, 0.8f, -0.45f};
        menu_set_scene_light(&M, ld, 0xff86b2d9, 0xff4a4a4a);
    }
    G.ext_menu = 1;
    G.save_fn = save_game;
    sv_init(&ST, SV_BACKEND_ALL, 0);
    g_has_save = sv_read_sync(&ST, &SG) == SV_OK;
    printf("ash-ds: boot save: %s (from %s, seq %u) \"%s\" zone %s\n", g_has_save ? "found" : "none",
           sv_src_name(ST.last_src), (unsigned)SG.seq, SG.name, sg_zone_name(SG.area, SG.block));
    menu_set_has_save(&M, g_has_save);
    menu_open(&M, MENU_TITLE);
    {
        struct mallinfo mi = mallinfo();
        printf("ash-ds: ready, heap in use %d KB, scenario %s\n", mi.uordblks / 1024, sc_name[sc]);
    }

    u64 t_last;
    sceRtcGetCurrentTick(&t_last);
    u32 tick_res = sceRtcGetTickResolution();
    float acc = 0, end_wait = 0;
    int last_plunges = 0, last_victory = 0, last_deaths = 0, play_frames = 0, fl_frames = 0;
    int shot_blk = 0, shot_par = 0, shot_rip = 0;
    int last_menu = -1, last_boss_bar = 0, last_dead = 0, combo_frames = 0;
    diag_event_sync("main loop start, scenario %s", sc_name[sc]);
    while (g_running) {
        diag_stage("main loop");
        u64 tn;
        sceRtcGetCurrentTick(&tn);
        acc += (float)(tn - t_last) / (float)tick_res;
        t_last = tn;
        int steps = B.s ? 1 : (int)(acc / ASY_DT);
        if (steps > 2) steps = 2;
        if (!B.s) acc -= steps * ASY_DT; else acc = 0;
        if (acc > ASY_DT) acc = ASY_DT;

        bot_tick();
        u32 buttons;
        if (B.s) buttons = B.menu_buttons;
        else { SceCtrlData pad; sceCtrlPeekBufferPositive(&pad, 1); buttons = pad.Buttons; }
        /* black box events (src/diag): menus, boss fight, death */
        if (menu_screen(&M) != last_menu) { last_menu = menu_screen(&M); diag_event("menu screen %d", last_menu); }
        if (g_in_game && G.boss_bar != last_boss_bar) {
            last_boss_bar = G.boss_bar;
            diag_event_sync("boss fight %s (boss hp %d)", last_boss_bar ? "START" : "end", G.has_boss ? (int)G.B.hp : 0);
        }
        if (g_in_game && G.pl.dead != last_dead) {
            last_dead = G.pl.dead;
            if (last_dead) diag_event_sync("player DEATH #%d (zone %d)", G.deaths, G.zone);
        }
        /* hardware self-test of the crash catcher: hold SELECT+L+R for 3 s (the hw-day1 design notes) */
        u32 combo = PSP_CTRL_SELECT | PSP_CTRL_LTRIGGER | PSP_CTRL_RTRIGGER;
        combo_frames = !B.s && (buttons & combo) == combo ? combo_frames + 1 : 0;
        if (combo_frames == 90) diag_test_crash(0);
        if (g_crash_at >= 0 && (int)g_frame >= g_crash_at) { g_crash_at = -1; diag_test_crash(g_crash_kind); }

        if (g_state == S_TITLE) {
            int ev = menu_tick(&M, buttons, ASY_DT);
            if (ev == MEV_NEW_GAME) {
                int asy_sc = B.s && B.s[B.i].op == O_ASYLUM ? (int)B.s[B.i].a : ASY_SC_NONE;
                for (int k = B.i; B.s && B.s[k].op != O_END; k++)   /* the scenario's Asylum autopilot */
                    if (B.s[k].op == O_ASYLUM) { asy_sc = (int)B.s[k].a; break; }
                new_game(asy_sc);
            } else if (ev == MEV_CONTINUE && g_has_save) {
                continue_game();
            } else menu_event(ev);
            sceGuStart(GU_DIRECT, g_list);
            sceGuClearColor(0xff000000);
            sceGuClearDepth(0);
            sceGuClear(GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT);
            if (g_state == S_TITLE) menu_draw(&M);
            present();
        } else {
            /* the game: pause on START, src/menu screens for bonfire and death, the crow -> Firelink */
            u32 pressed = buttons & ~M.prev;
            if (!menu_screen(&M) && (pressed & PSP_CTRL_START) && !prg_locked(&G.prg) && !G.pl.dead) {
                view_fill();
                menu_open(&M, MENU_PAUSE);
            }
            if (G.prg.phase == PRG_REST && G.prg.sub == 1 && !menu_screen(&M) && !g_bf_open) {
                view_fill();
                menu_open(&M, MENU_BONFIRE);
                g_bf_open = 1;
                printf("ash-ds: f%u bonfire menu (src/menu)\n", (unsigned)g_frame);
            }
            if (G.prg.phase != PRG_REST) g_bf_open = 0;
            if (G.prg.phase == PRG_DYING && !g_died_open) {
                menu_open(&M, MENU_DIED);
                g_died_open = (int)g_frame;
            }
            if (g_died_open > 0 && (int)g_frame == g_died_open + 75) shot("20_died");   /* the red text is up */
            if (G.prg.phase == PRG_PLAY) g_died_open = 0;
            int blocked = menu_blocks_game(&M);
            int ev = menu_tick(&M, buttons, ASY_DT);
            if (blocked || menu_blocks_game(&M)) g_swallow |= game_pad().buttons;   /* see g_swallow */
            if (ev) menu_event(ev);
            if (g_state != S_PLAY) continue;
            if (!g_pipe) sim_steps(steps);
            else g_pipe_steps = steps;
            save_poll();
            /* automatic named frames of the Asylum run */
            play_frames++;
            if (G.zone == ASY_ZONE_ASYLUM) {
                if (play_frames == 45) shot("06_cell_start");
                if (G.plunges > last_plunges) { last_plunges = G.plunges; shot("15_plunge_hit"); }
                if (G.victory && !last_victory) last_victory = (int)g_frame;
                if (last_victory > 0 && (int)g_frame == last_victory + 45) shot("16_victory");
                if (G.deaths > last_deaths) last_deaths = G.deaths;
                if (G.blocks && !shot_blk) { shot_blk = 1; shot("11_block"); }
                if (G.parries && !shot_par) { shot_par = (int)g_frame; shot("12_parry"); }
                if (G.ripostes && !shot_rip) shot_rip = (int)g_frame;
                if (shot_rip > 0 && (int)g_frame == shot_rip + 18) shot("13_riposte");
            } else fl_frames++;
            sceGuStart(GU_DIRECT, g_list);
            draw_game(1);
            present();
            if (g_pipe) { g_pipe_steps = 0; }
            if (G.zone == ASY_ZONE_ASYLUM && G.end_demo && G.end_t > 1.6f) {   /* the crow carries the player away */
                printf("ash-ds: f%u the crow: Asylum -> Firelink Shrine\n", (unsigned)g_frame);
                save_game(&G, "zone change (leave Asylum)");
                while (g_saving) { save_poll(); sceDisplayWaitVblankStart(); }
                if (enter_firelink() == 0) save_game(&G, "zone change (enter Firelink)");
                play_frames = 0;
            }
            if (g_frame % 150 == 0)
                printf("ash-ds: f%u zone %d t=%.0fs st=%s pos=(%.2f %.2f %.2f) hp=%.0f/%.0f souls=%d estus=%d menu=%d inv=%d\n",
                       (unsigned)g_frame, G.zone, G.time, asy_pst_name[G.pl.st], G.pl.pos[0], G.pl.pos[1], G.pl.pos[2],
                       G.pact.hp, G.pact.hp_max, (int)G.prg.souls, (int)G.prg.estus, menu_screen(&M), G.inv.n);
        }
        if (B.s && B.done) { end_wait += ASY_DT; if (end_wait > 1.0f && !g_saving) break; }
        if (B.s && g_max_frames > 0 && (int)g_frame >= g_max_frames) break;
    }
    while (g_saving) { save_poll(); sceDisplayWaitVblankStart(); }
    struct mallinfo mi = mallinfo();
    printf("DS:{\"scenario\":\"%s\",\"frames\":%u,\"zone\":%d,\"done\":%d,\"bot_warps\":%d,\"asy_warps\":%d,"
           "\"victory\":%d,\"deaths\":%d,\"kills\":%d,\"plunges\":%d,\"parries\":%d,\"ripostes\":%d,\"blocks\":%d,"
           "\"rests\":%d,\"bonfires\":%d,\"items\":%d,\"inv\":%d,\"souls\":%d,\"saves\":%d,\"has_save\":%d,"
           "\"fl_frames\":%d,\"heap_kb\":%u}\n",
           sc_name[sc], (unsigned)g_frame, G.zone, B.done, B.warps, G.au.warps, G.victory, G.deaths, G.kills, G.plunges,
           G.parries, G.ripostes, G.blocks, G.rests, G.bonfires_lit, G.items, G.inv.n, (int)G.prg.souls, G.saves,
           g_has_save, fl_frames, (unsigned)(mi.uordblks / 1024));
    sceGuTerm();
    sceKernelExitGame();
    return 0;
}
