/*
 * Ash combat test EBOOT (src/combat/test): the Elite Knight against an Asylum hollow (c2500, NpcParam 250022) and the
 * Asylum Demon (c2232) in the upper hall of m18_01, driven by src/combat only - no game code, no pad: a scripted
 * autopilot for PPSSPPHeadless. Scenes, in order:
 *   1 BLOCK     the hollow swings a3000 into the raised Knight Shield: stamina loss, guard_hit, the hollow is repelled
 *   2 PARRY     the knight parries a3000 (a48_5100 box against the TAE parry-possible flag), the hollow drops into
 *               a2100, the knight presses R1 -> cb_try_throw -> riposte a23_3201 / victim a2510, damage at e304
 *   3 BACKSTAB  the hollow turned away, R1 from behind -> a23_3400 / a2500
 *   4 RUN+HEAVY running R1 a23_3500 from a sprint (hollow staggers into a2004), then R2 a23_3300
 *   5 STAGGER   the hollow hits the unguarded knight until the Elite set's poise 46 breaks (player hit reaction)
 *   6 PLUNGE    the knight drops from the balcony height (8.7 m) past the Asylum Demon: a23_3800 -> 3801 -> 3810,
 *               one AttackType-62 hit
 * Positions and the fall are scripted here [придумал]: no collision except the floor height from arena.pcol.
 *
 * Files next to the EBOOT (src/combat/test/shoot-cbtest.sh stages them): scene.ashm (arena_nochars), arena.pcol,
 * combat.ashcb, knight.ashc/.ashanim, knight_combat.ashanim, hollow.ashc/.ashanim, hollow_combat.ashanim,
 * c2232.ashc/.ashanim, demo.txt "<frames>". Stdout "ash-cbtest: ...". Frame dumps fr_NNNN.raw on combat events.
 * Emulated timings are not hardware timings.
 */
#include <pspdebug.h>
#include <pspdisplay.h>
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
#include "combat.h"
#include "pcol.h"
#include "scene.h"

PSP_MODULE_INFO("Ash Combat", 0, 0, 1);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);
PSP_HEAP_SIZE_KB(-1024);

#define BUF_WIDTH 512
#define SCR_WIDTH 480
#define SCR_HEIGHT 272
#define DT (1.0f / 30.0f)
#define PI 3.14159265f
#define BLEND 0.12f
#define MAX_DUMPS 60

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

/* ------------------------------------------------------------------ animated body */
typedef struct {
    Chr chr;
    AnimSet set[2];
    int n_set;
    int cs, ci, ps, pi;           /* current / previous clip (set, index) */
    float t, pt, blend;
    int hold;
    float pos[3], yaw;
    BonePose __attribute__((aligned(16))) pa[ANIM_MAX_BONES], pb[ANIM_MAX_BONES];
    ScePspFMatrix4 __attribute__((aligned(16))) world[ANIM_MAX_BONES], skin[ANIM_MAX_BONES];
    CbActor act;
    int cbi;
    u32 last_anim;
} Body;

static Body g_k, g_h, g_d;

static int load(const char *name, u8 **f)
{
    int n = ash_load_file(name, f, NULL, 0);
    if (n < 0) printf("ash-cbtest: %s missing\n", name);
    return n;
}

static int body_load(Body *b, const char *chr, const char *a0, const char *a1)
{
    u8 *f;
    int n = load(chr, &f);
    if (n < 0 || chr_load(&b->chr, f, n) < 0) return -1;
    b->n_set = 0;
    n = load(a0, &f);
    if (n < 0 || anim_load(&b->set[b->n_set++], f, n) < 0) return -1;
    if (a1) {
        n = load(a1, &f);
        if (n > 0 && anim_load(&b->set[b->n_set], f, n) == 0) b->n_set++;
    }
    b->cs = 0; b->ci = anim_find(&b->set[0], "idle"); b->ps = -1;
    return 0;
}

static int clip_by_id(const Body *b, u32 id, int *set)
{
    for (int s = b->n_set - 1; s >= 0; s--)
        for (u32 i = 0; i < b->set[s].hdr->n_clips; i++)
            if (b->set[s].clips[i].anim_id == id) { *set = s; return (int)i; }
    return -1;
}

static int clip_by_name(const Body *b, const char *name, int *set)
{
    for (int s = 0; s < b->n_set; s++) {
        int i = anim_find(&b->set[s], name);
        if (i >= 0) { *set = s; return i; }
    }
    return -1;
}

static float dur(const Body *b, int s, int i) { return i >= 0 ? anim_duration(&b->set[s], i) : 0; }
static int loops(const Body *b, int s, int i) { return i >= 0 && (b->set[s].clips[i].flags & CLIP_LOOP); }
static u32 cur_id(const Body *b) { return b->ci >= 0 ? b->set[b->cs].clips[b->ci].anim_id : 0; }

static void play_clip(Body *b, int s, int i, int hold)
{
    if (i < 0) return;
    b->ps = b->cs; b->pi = b->ci; b->pt = b->t;
    b->cs = s; b->ci = i; b->t = 0; b->blend = 0; b->hold = hold;
}

/* play by anim id (combat ids: knight 233000 / 1 / 710 ..., hollow 3000 / 2100 ...); 0 if the clip is not cooked */
static int play_id(Body *b, u32 id, int hold)
{
    int s, i = clip_by_id(b, id, &s);
    if (i < 0) { printf("ash-cbtest: no clip for anim %u\n", (unsigned)id); return 0; }
    play_clip(b, s, i, hold);
    return 1;
}

static int play_name(Body *b, const char *name, int hold)
{
    int s, i = clip_by_name(b, name, &s);
    if (i < 0) return 0;
    if (s == b->cs && i == b->ci) return 1;
    play_clip(b, s, i, hold);
    return 1;
}

static float clip_t(const Body *b)
{
    float d = dur(b, b->cs, b->ci);
    if (loops(b, b->cs, b->ci) && !b->hold) return d > 0 ? fmodf(b->t, d) : 0;
    return b->t > d ? d : b->t;
}

static int body_done(const Body *b) { return !loops(b, b->cs, b->ci) && b->t >= dur(b, b->cs, b->ci); }

/* advance by dt (0 while in hit stop), root motion in world space */
static void body_step(Body *b, float dt, int root)
{
    if (dt <= 0) return;
    if (root && b->ci >= 0 && (loops(b, b->cs, b->ci) || b->t < dur(b, b->cs, b->ci))) {
        float r[4];
        anim_root_delta(&b->set[b->cs], b->ci, b->t, b->t + dt, r);
        float s = sinf(b->yaw), c = cosf(b->yaw);
        b->pos[0] += c * r[0] + s * r[2];
        b->pos[2] += -s * r[0] + c * r[2];
    }
    b->t += dt; b->pt += dt; b->blend += dt;
}

static void body_pose(Body *b)
{
    int n = b->set[0].n_bones;
    anim_sample(&b->set[b->cs], b->ci, clip_t(b), b->pa);
    if (b->ps >= 0 && b->pi >= 0 && b->blend < BLEND) {
        float pd = dur(b, b->ps, b->pi), pt = b->pt;
        if (!loops(b, b->ps, b->pi) && pt > pd) pt = pd;
        anim_sample(&b->set[b->ps], b->pi, pt, b->pb);
        anim_blend(n, b->pb, b->pa, b->blend / BLEND, b->pa);
    }
    anim_skin_matrices(n, b->chr.parent, b->pa, b->chr.inv_bind, b->world, b->skin);
}

static int bone_lookup(void *ctx, const char *name) { return chr_bone((const Chr *)ctx, name); }

/* body -> combat actor (after body_pose): pos, yaw, model-space bones, anim id/time */
static void body_to_actor(Body *b)
{
    CbActor *a = &b->act;
    memcpy(a->pos, b->pos, 12);
    a->yaw = b->yaw;
    a->bones = (const float *)b->world;
    a->n_bones = b->set[0].n_bones;
    u32 id = cur_id(b);
    if (id != b->last_anim) { a->anim_t_prev = 0; b->last_anim = id; }
    a->anim_id = id;
    a->anim_t = clip_t(b);
}

/* ------------------------------------------------------------------ helpers */
static float yaw_to(const float *from, const float *to) { return atan2f(-(to[0] - from[0]), -(to[2] - from[2])); }
static float flat_dist(const float *a, const float *b)
{
    float dx = a[0] - b[0], dz = a[2] - b[2];
    return sqrtf(dx * dx + dz * dz);
}
static void place(Body *b, float x, float y, float z, float yaw) { b->pos[0] = x; b->pos[1] = y; b->pos[2] = z; b->yaw = yaw; }

static void dump_frame(void *draw, int n)
{
    char name[64];
    snprintf(name, sizeof(name), "fr_%04d.raw", n);
    SceUID fd = sceIoOpen(name, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0) return;
    sceIoWrite(fd, (void *)(0x44000000 | (u32)draw), BUF_WIDTH * SCR_HEIGHT * 2);
    sceIoClose(fd);
}

typedef struct { float x, y, z; } V3;
typedef struct { u32 c; float x, y, z; } LineV;

static void draw_shapes(const CbWorld *w)
{
    CbShape sh[48];
    int n = combat_debug_shapes(w, sh, 48), m = 0;
    for (int i = 0; i < n; i++) m += sh[i].kind == 1;
    if (!m) return;
    LineV *v = (LineV *)sceGuGetMemory(sizeof(LineV) * 8 * m);
    int k = 0;
    for (int i = 0; i < n; i++) {
        if (sh[i].kind != 1) continue;
        u32 c = 0xff2040ff;
        float r = sh[i].r;
        v[k++] = (LineV){c, sh[i].a[0], sh[i].a[1], sh[i].a[2]};
        v[k++] = (LineV){c, sh[i].b[0], sh[i].b[1], sh[i].b[2]};
        v[k++] = (LineV){c, sh[i].b[0] - r, sh[i].b[1], sh[i].b[2]};
        v[k++] = (LineV){c, sh[i].b[0] + r, sh[i].b[1], sh[i].b[2]};
        v[k++] = (LineV){c, sh[i].b[0], sh[i].b[1] - r, sh[i].b[2]};
        v[k++] = (LineV){c, sh[i].b[0], sh[i].b[1] + r, sh[i].b[2]};
        v[k++] = (LineV){c, sh[i].b[0], sh[i].b[1], sh[i].b[2] - r};
        v[k++] = (LineV){c, sh[i].b[0], sh[i].b[1], sh[i].b[2] + r};
    }
    sceGuDisable(GU_DEPTH_TEST);
    sceGuDisable(GU_TEXTURE_2D);
    sceGumMatrixMode(GU_MODEL);
    sceGumLoadIdentity();
    sceGumDrawArray(GU_LINES, GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D, k, 0, v);
    sceGuEnable(GU_TEXTURE_2D);
    sceGuEnable(GU_DEPTH_TEST);
}

static void draw_body(Body *b)
{
    sceGumMatrixMode(GU_MODEL);
    sceGumLoadIdentity();
    ScePspFVector3 t = {b->pos[0], b->pos[1], b->pos[2]};
    sceGumTranslate(&t);
    sceGumRotateY(b->yaw);
    chr_draw(&b->chr, b->skin);
}

/* ------------------------------------------------------------------ script */
enum { SC_BLOCK, SC_PARRY, SC_BACKSTAB, SC_RUNHEAVY, SC_STAGGER, SC_PLUNGE, SC_END };
static const char *const sc_name[] = {"1 BLOCK + REPEL", "2 PARRY -> RIPOSTE", "3 BACKSTAB", "4 RUN R1 + HEAVY R2",
                                      "5 PLAYER POISE", "6 PLUNGE (DEMON)", "END"};
static const int sc_len[] = {150, 260, 210, 190, 330, 200};

static CbWorld g_w;
static CbData g_cbd;
static float g_floor = 198.06f;
static char g_last[80];
static int g_counts[16];
static float g_plunge_dmg;

static void reset_actors(int with_demon)
{
    cb_actor_init_player(&g_k.act, &g_cbd);
    cb_actor_bind(&g_k.act, &g_cbd, bone_lookup, &g_k.chr);
    cb_actor_init_npc(&g_h.act, &g_cbd, 250022, "c2500");
    cb_actor_bind(&g_h.act, &g_cbd, bone_lookup, &g_h.chr);
    cb_actor_init_npc(&g_d.act, &g_cbd, 223200, "c2232");
    cb_actor_bind(&g_d.act, &g_cbd, bone_lookup, &g_d.chr);
    if (!with_demon) g_d.act.dead = 1;     /* out of the fight */
    else g_h.act.dead = 1;
    g_k.hold = g_h.hold = 0;
    play_name(&g_k, "idle", 0);
    play_name(&g_h, "idle", 0);
    play_name(&g_d, "idle", 0);
}

/* cb_try_throw moved the victim onto the attacker's throw dummy: the bodies follow */
static void throw_snap(void)
{
    g_h.pos[0] = g_h.act.pos[0]; g_h.pos[2] = g_h.act.pos[2]; g_h.yaw = g_h.act.yaw;
}

/* the knight starts an action through the combat module (stamina), then plays it */
static int knight_act(u32 id)
{
    if (!cb_begin_anim(&g_w, &g_k.act, id)) { printf("ash-cbtest: action %u refused (stamina %.1f)\n", (unsigned)id, g_k.act.sp); return 0; }
    return play_id(&g_k, id, 0);
}
static int hollow_act(u32 id)
{
    cb_begin_anim(&g_w, &g_h.act, id);
    return play_id(&g_h, id, 0);
}

int main(void)
{
    int th = sceKernelCreateThread("cb_thread", cb_thread, 0x11, 0xFA0, 0, 0);
    if (th >= 0) sceKernelStartThread(th, 0, 0);

    Scene sc;
    u8 *f;
    int n = load("scene.ashm", &f);
    if (n < 0 || scene_load(&sc, f, n) < 0) { sceKernelExitGame(); return 0; }
    if (body_load(&g_k, "knight.ashc", "knight.ashanim", "knight_combat.ashanim") < 0 ||
        body_load(&g_h, "hollow.ashc", "hollow.ashanim", "hollow_combat.ashanim") < 0 ||
        body_load(&g_d, "c2232.ashc", "c2232.ashanim", NULL) < 0) {
        printf("ash-cbtest: characters missing\n"); sceKernelExitGame(); return 0;
    }
    n = load("combat.ashcb", &f);
    if (n < 0 || cb_data_load(&g_cbd, f, n) < 0) { printf("ash-cbtest: combat.ashcb bad\n"); sceKernelExitGame(); return 0; }
    {
        Pcol col;
        u8 *fc;
        int nc = ash_load_file("arena.pcol", &fc, NULL, 0);
        float h;
        if (nc > 0 && pcol_load(&col, fc, nc) == 0 && pcol_floor(&col, -3.0f, -18.0f, 202.0f, 194.0f, 0.7f, &h) >= 0)
            g_floor = h;
        printf("ash-cbtest: floor y %.3f (%s)\n", g_floor, nc > 0 ? "arena.pcol" : "default");
    }
    cb_world_init(&g_w, &g_cbd);
    g_k.cbi = cb_world_add(&g_w, &g_k.act);
    g_h.cbi = cb_world_add(&g_w, &g_h.act);
    g_d.cbi = cb_world_add(&g_w, &g_d.act);
    reset_actors(0);
    printf("ash-cbtest: knight hp %.0f sp %.0f poise %.0f; hollow hp %.0f; demon hp %.0f; clips knight %u+%u hollow %u+%u\n",
           g_k.act.hp, g_k.act.sp, g_k.act.poise_max, g_h.act.hp, g_d.act.hp_max, (unsigned)g_k.set[0].hdr->n_clips,
           (unsigned)(g_k.n_set > 1 ? g_k.set[1].hdr->n_clips : 0), (unsigned)g_h.set[0].hdr->n_clips,
           (unsigned)(g_h.n_set > 1 ? g_h.set[1].hdr->n_clips : 0));

    int frames = 1400;
    {
        u8 *buf = NULL;
        if (ash_load_file("demo.txt", &buf, NULL, 0) > 0) { sscanf((const char *)buf, "%d", &frames); free(buf); }
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

    void *draw = fbp0;
    int dumps = 0, dump_at[8] = {0}, n_dump_at = 0;
    const u32 fog = 0xff1a1714;
    int sc_i = -1, pf = 0, sc_start = 0;
    float vy = 0;          /* plunge fall speed */
    int plunge_state = 0, parry_f = -1, riposte_sent = 0, staggered = 0, hits_on_k = 0;
    const float HX = -3.0f, HZ = -18.0f;   /* the hollow's spot, between the arena entrance and the demon */

    for (int fr = 0; fr < frames && g_running; fr++) {
        /* ---------------- scene switch */
        if (sc_i < 0 || (sc_i < SC_END && fr - sc_start >= sc_len[sc_i])) {
            sc_i++;
            sc_start = fr;
            pf = 0;
            if (sc_i >= SC_END) break;
            reset_actors(sc_i == SC_PLUNGE);
            parry_f = -1; riposte_sent = 0; staggered = 0; hits_on_k = 0; plunge_state = 0; vy = 0;
            float y = g_floor;
            switch (sc_i) {
            case SC_BLOCK:
            case SC_PARRY:
            case SC_STAGGER:
                place(&g_h, HX, y, HZ, PI);                 /* hollow faces +z, toward the knight */
                place(&g_k, HX, y, HZ + 1.25f, 0);          /* knight faces -z */
                break;
            case SC_BACKSTAB:
                place(&g_h, HX, y, HZ, 0);                  /* hollow faces -z: its back to the knight */
                place(&g_k, HX + 0.3f, y, HZ + 3.0f, 0);
                break;
            case SC_RUNHEAVY:
                place(&g_h, HX, y, HZ, PI);
                place(&g_k, HX, y, HZ + 9.0f, 0);
                break;
            case SC_PLUNGE:
                place(&g_d, -3.0f, y, -29.54f, PI);         /* arena.spawn: c2232 at its MSB spot */
                place(&g_k, -4.6f, y + 8.7f, -32.8f, PI);   /* balcony height (bosstest: 206.8), behind the demon */
                place(&g_h, 40.0f, y, 40.0f, 0);
                break;
            }
            printf("ash-cbtest: f%d SCENE %s\n", fr, sc_name[sc_i]);
            g_last[0] = 0;
            if (n_dump_at < 8) dump_at[n_dump_at++] = fr + 8;
        }

        /* ---------------- autopilot */
        u32 hid = cur_id(&g_h);
        switch (sc_i) {
        case SC_BLOCK:
            if (pf == 2) play_name(&g_k, "guard", 0);
            g_k.act.guard_input = pf >= 2 && cur_id(&g_k) == 1;
            if (pf >= 2 && body_done(&g_k)) play_name(&g_k, "guard", 0);   /* back to guard after guard_hit */
            if (pf == 10) hollow_act(3000);
            break;
        case SC_PARRY:
            if (pf == 10) hollow_act(3000);
            /* the parry box (a48_5100 frames 0-7) must cover the hollow's parry-possible frame 28 of a3000 */
            if (pf == 10 + 24) knight_act(485100);
            if (parry_f >= 0 && !riposte_sent && pf >= parry_f + 8) {
                int v = cb_try_throw(&g_w, g_k.cbi);
                if (v >= 0) throw_snap();
                printf("ash-cbtest: f%d R1 after the parry: cb_try_throw -> %d (dist %.2f m)\n", fr, v, flat_dist(g_k.pos, g_h.pos));
                riposte_sent = v >= 0 || pf > parry_f + 30;
            }
            break;
        case SC_BACKSTAB:
            if (pf < 60) {
                if (flat_dist(g_k.pos, g_h.pos) > 0.95f) { play_name(&g_k, "walk", 0); g_k.yaw = yaw_to(g_k.pos, g_h.pos); }
                else play_name(&g_k, "idle", 0);
            }
            if (pf == 60) {
                int v = cb_try_throw(&g_w, g_k.cbi);
                if (v >= 0) throw_snap();
                printf("ash-cbtest: f%d R1 behind the hollow: cb_try_throw -> %d (dist %.2f m)\n", fr, v, flat_dist(g_k.pos, g_h.pos));
            }
            break;
        case SC_RUNHEAVY:
            if (pf < 90 && cur_id(&g_k) != 233500 && cur_id(&g_k) != 233300) {
                g_k.yaw = yaw_to(g_k.pos, g_h.pos);
                if (flat_dist(g_k.pos, g_h.pos) > 3.2f) play_name(&g_k, "run", 0);
                else knight_act(cb_attack_anim(&g_w, &g_k.act, CB_ACT_RUN_R1, 0));
            }
            if (cur_id(&g_k) == 233500 && g_k.t > 1.6f && !g_h.act.dead) {
                g_k.yaw = yaw_to(g_k.pos, g_h.pos);
                knight_act(cb_attack_anim(&g_w, &g_k.act, CB_ACT_R2, 0));
            }
            break;
        case SC_STAGGER:
            /* the hollow keeps swinging a3000 at the standing knight */
            if (!g_h.act.dead && (pf == 10 || (hid == 3000 && body_done(&g_h)) || (hid != 3000 && hid != 0 && body_done(&g_h))))
                hollow_act(3000);
            if (cur_id(&g_k) != 0 && body_done(&g_k)) play_name(&g_k, "idle", 0);
            break;
        case SC_PLUNGE:
            if (plunge_state == 0 && pf == 6) {
                knight_act(cb_attack_anim(&g_w, &g_k.act, CB_ACT_PLUNGE, 0));
                plunge_state = 1;
                printf("ash-cbtest: f%d R1 while falling: plunge a23_3800 (y %.2f)\n", fr, g_k.pos[1]);
            }
            if (plunge_state == 1 && body_done(&g_k)) knight_act(cb_attack_anim(&g_w, &g_k.act, CB_ACT_PLUNGE_FALL, 0));
            break;
        }
        /* falling (plunge scene): gravity, 1.2 m/s drift toward the demon, landing */
        if (sc_i == SC_PLUNGE && plunge_state >= 0 && g_k.pos[1] > g_floor) {
            vy -= 9.8f * DT;
            g_k.pos[1] += vy * DT;
            g_k.pos[2] += 1.2f * DT;
            if (g_k.pos[1] <= g_floor) {
                g_k.pos[1] = g_floor;
                knight_act(cb_attack_anim(&g_w, &g_k.act, CB_ACT_PLUNGE_LAND, 0));
                printf("ash-cbtest: f%d landed (fall %.2f s, %.1f m/s): plunge_land a23_3810, stamina %.1f\n", fr,
                       pf / 30.0f, -vy, g_k.act.sp);
                plunge_state = -1;
            }
        }
        /* reactions requested by combat */
        Body *bs[3] = {&g_k, &g_h, &g_d};
        for (int i = 0; i < 3; i++) {
            CbActor *a = &bs[i]->act;
            if (!a->req_anim) continue;
            u32 id = a->req_anim;
            a->req_anim = 0;
            if (bs[i] == &g_d) continue;   /* the demon here is a target only */
            int s, ci = clip_by_id(bs[i], id, &s);
            if (ci < 0 && bs[i] == &g_h) ci = clip_by_name(bs[i], a->dead ? "death" : "hit", &s);
            int hold = a->dead || id == 2500 || id == 2510 || id == 6000;
            if (ci >= 0) { play_clip(bs[i], s, ci, hold); cb_begin_anim(&g_w, a, cur_id(bs[i])); }
            else printf("ash-cbtest: f%d actor %d reaction %u: no clip\n", fr, i, (unsigned)id);
        }
        /* idle when an action ended */
        if (body_done(&g_h) && !g_h.hold && !g_h.act.dead && cur_id(&g_h) != 0 && sc_i != SC_STAGGER) play_name(&g_h, "idle", 0);
        if (body_done(&g_k) && !g_k.hold && sc_i != SC_BLOCK && !(sc_i == SC_PLUNGE && plunge_state == 1)) play_name(&g_k, "idle", 0);
        if (g_h.act.dead && body_done(&g_h) && g_h.hold && cur_id(&g_h) != 2200) {
            int s, ci = clip_by_name(&g_h, "death", &s);
            if (ci >= 0) play_clip(&g_h, s, ci, 1);
        }

        /* ---------------- animation, root motion, combat */
        for (int i = 0; i < 3; i++) {
            Body *b = bs[i];
            float adv = b->act.hitstop > 0 ? 0 : DT;
            int root = !(b == &g_k && sc_i == SC_PLUNGE);
            float p0[3];
            memcpy(p0, b->pos, 12);
            body_step(b, adv, root);
            /* keep bodies apart (hollow 0.4 + knight 0.4 [версия]) except during throws */
            if (b != &g_d && !(b->act.flags & CB_ST_THROWN) && b->act.throw_partner < 0) {
                Body *o = b == &g_k ? &g_h : &g_k;
                float d = flat_dist(b->pos, o->pos);
                if (d < 0.75f && flat_dist(p0, o->pos) >= d) memcpy(b->pos, p0, 12);
            }
            if (b->act.push_t > 0 && b != &g_d) { b->pos[0] += b->act.push[0] * DT; b->pos[2] += b->act.push[2] * DT; }
            body_pose(b);
            body_to_actor(b);
        }
        g_k.act.guard_input = sc_i == SC_BLOCK && cur_id(&g_k) == 1;
        combat_update(&g_w, DT);
        for (int k = 0; k < g_w.n_ev; k++) {
            const CbEvent *e = &g_w.ev[k];
            static const char *const nm[] = {"", "HIT", "BLOCK", "GUARD_BREAK", "PARRY", "STAGGER", "DEATH", "THROW",
                                             "THROW_HIT", "INVULN", "REPEL"};
            const char *who[3] = {"knight", "hollow", "demon"};
            CbActor *v = g_w.actor[e->victim];
            if (e->type < 16) g_counts[e->type]++;
            snprintf(g_last, sizeof g_last, "%s %s->%s %.1f dmg%s", nm[e->type], who[e->attacker], who[e->victim],
                     e->damage, e->atk_type == CB_ATTACK_PLUNGE ? " PLUNGE" : "");
            printf("ash-cbtest: f%d [%s] EV %s: %s -> %s, dmg %.1f, stamina -%.1f, anim %u, atk_type %u, victim hp %.1f "
                   "sp %.1f\n", fr, sc_name[sc_i], nm[e->type], who[e->attacker], who[e->victim], e->damage, e->stamina,
                   (unsigned)e->anim, e->atk_type, v->hp, v->sp);
            if (e->type == CB_EV_PARRY && parry_f < 0) parry_f = pf;
            if (e->type == CB_EV_STAGGER && e->victim == g_k.cbi) staggered++;
            if (e->type == CB_EV_HIT && e->victim == g_k.cbi) hits_on_k++;
            if (e->type == CB_EV_HIT && e->atk_type == CB_ATTACK_PLUNGE) g_plunge_dmg = e->damage;
            {
                int dup = 0;
                for (int q = 0; q < n_dump_at; q++) dup |= dump_at[q] == fr + 2;
                if (!dup && n_dump_at < 7) { dump_at[n_dump_at++] = fr + 2; dump_at[n_dump_at++] = fr + 12; }
            }
        }
        if (sc_i == SC_STAGGER && staggered && pf > 0 && hits_on_k > 0 && g_h.act.dead == 0 && fr - sc_start > 20 &&
            staggered == 1 && cur_id(&g_k) != 0) { /* keep going: the scene timer ends it */ }

        /* ---------------- camera: side view of the pair (plunge: wide, from the side of the demon) */
        float cam_eye[3], cam_at[3];
        {
            Body *o = sc_i == SC_PLUNGE ? &g_d : &g_h;
            float mid[3] = {(g_k.pos[0] + o->pos[0]) * 0.5f, (g_k.pos[1] + o->pos[1]) * 0.5f, (g_k.pos[2] + o->pos[2]) * 0.5f};
            float dx = o->pos[0] - g_k.pos[0], dz = o->pos[2] - g_k.pos[2], l = sqrtf(dx * dx + dz * dz) + 1e-3f;
            float side = sc_i == SC_PLUNGE ? 12.0f : 4.2f, up = sc_i == SC_PLUNGE ? 2.5f : 1.4f;
            if (sc_i == SC_RUNHEAVY) side = 5.5f;
            /* perpendicular, slightly in front of the knight */
            cam_eye[0] = mid[0] + dz / l * side - dx / l * 1.2f;
            cam_eye[2] = mid[2] - dx / l * side - dz / l * 1.2f;
            cam_eye[1] = (sc_i == SC_PLUNGE ? g_floor + 3.5f : mid[1]) + up;
            cam_at[0] = mid[0]; cam_at[2] = mid[2];
            cam_at[1] = sc_i == SC_PLUNGE ? g_floor + 4.0f : mid[1] + 1.0f;
            if (sc_i == SC_PLUNGE) {   /* north-west of the demon, clear of the pillars (ray test on arena.pcol) [придумал] */
                cam_eye[0] = -9.0f; cam_eye[1] = g_floor + 5.0f; cam_eye[2] = -24.0f;
                cam_at[0] = -3.8f; cam_at[1] = g_floor + 3.5f; cam_at[2] = -31.0f;
            }
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
        int nbat = 0;
        scene_draw(&sc, &vp, &nbat);
        sceGuEnable(GU_LIGHTING);
        sceGuColor(0xffffffff);
        sceGuAmbientColor(0xffffffff);
        draw_body(&g_k);
        if (sc_i == SC_PLUNGE) draw_body(&g_d);
        else draw_body(&g_h);
        sceGuDisable(GU_LIGHTING);
        sceGuDisable(GU_FOG);
        draw_shapes(&g_w);
        sceGuFinish();
        sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);

        pspDebugScreenSetBase((u32 *)(0x44000000 | (u32)draw));
        pspDebugScreenSetXY(0, 0);
        pspDebugScreenSetTextColor(0xff9adfff);
        pspDebugScreenPrintf("ASH COMBAT f%-4d %s", fr, sc_name[sc_i]);
        pspDebugScreenSetXY(0, 1);
        pspDebugScreenPrintf("knight hp %3.0f sp %3.0f poise %2.0f a%u", g_k.act.hp, g_k.act.sp, g_k.act.poise,
                             (unsigned)cur_id(&g_k));
        pspDebugScreenSetXY(0, 2);
        if (sc_i == SC_PLUNGE) pspDebugScreenPrintf("demon hp %3.0f/%3.0f  y %.1f", g_d.act.hp, g_d.act.hp_max, g_k.pos[1] - g_floor);
        else pspDebugScreenPrintf("hollow hp %3.0f/%3.0f a%u", g_h.act.hp, g_h.act.hp_max, (unsigned)cur_id(&g_h));
        pspDebugScreenSetXY(0, 3);
        pspDebugScreenSetTextColor(0xff60ffff);
        pspDebugScreenPrintf("%s", g_last);
        for (int i = 0; i < n_dump_at; i++)
            if (dump_at[i] == fr && dumps < MAX_DUMPS) {
                dump_frame(draw, dumps);
                printf("ash-cbtest: dump fr_%04d = frame %d (%s)\n", dumps, fr, sc_name[sc_i]);
                dumps++;
            }
        /* drop consumed dump requests */
        {
            int m = 0;
            for (int i = 0; i < n_dump_at; i++) if (dump_at[i] > fr) dump_at[m++] = dump_at[i];
            n_dump_at = m;
        }
        if (fr % 30 == 0)
            printf("ash-cbtest: f%d %s knight (%.2f %.2f %.2f) a%u t%.2f hp %.0f sp %.1f | hollow (%.2f %.2f) a%u t%.2f hp %.0f "
                   "| demon hp %.0f [emulated]\n", fr, sc_name[sc_i], g_k.pos[0], g_k.pos[1], g_k.pos[2],
                   (unsigned)cur_id(&g_k), g_k.t, g_k.act.hp, g_k.act.sp, g_h.pos[0], g_h.pos[2], (unsigned)cur_id(&g_h),
                   g_h.t, g_h.act.hp, g_d.act.hp);
        sceDisplayWaitVblankStart();
        draw = sceGuSwapBuffers();
        pf++;
    }
    printf("ash-cbtest: done hits %d blocks %d repels %d parries %d staggers %d deaths %d throws %d throw_hits %d "
           "plunge_dmg %.1f dumps %d\n", g_counts[CB_EV_HIT], g_counts[CB_EV_BLOCK], g_counts[CB_EV_REPEL],
           g_counts[CB_EV_PARRY], g_counts[CB_EV_STAGGER], g_counts[CB_EV_DEATH], g_counts[CB_EV_THROW],
           g_counts[CB_EV_THROW_HIT], g_plunge_dmg, dumps);
    sceKernelExitGame();
    return 0;
}
