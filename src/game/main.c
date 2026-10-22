/*
 * Ash M4a "arena: world and target": the Asylum Demon's arena (upper hall of m18_01, .ashm without baked characters)
 * with its real Havok collision (.pcol: capsule controller, camera sphere cast), the Elite Knight skinned on the GE
 * and animated, the Asylum Demon (c2232, idle, 31 of 151 bones) as the lock-on target; free / lock-on camera for the
 * PSP's single stick. The same EBOOT runs the pit under it (Stray Demon c2230) with the "hall" cook.
 *
 * Files next to the EBOOT (or umd0:/, host0:/, ms0:/ash/):
 *   scene.ashm      tools/cook_scene.py --no-chars (or --no-knight: then the baked demon stays in the scene)
 *   knight.ashc     tools/cook_knight.py
 *   knight.ashanim  tools/cook_knight.py
 *   demon.ashc      tools/cook_demon.py --chr c2232 (optional: lock-on target; c2230 for the pit)
 *   demon.ashanim   tools/cook_demon.py
 *   scene.pcol      tools/cook_collision.py (optional: without it the floor is flat and there are no walls)
 *   scene.spawn     tools/cook_collision.py: player spot + target position from the MSB (else the pit defaults)
 *   demo.txt        optional: autopilot (headless has no pad). Content "<dump_every> <max_frames>", e.g. "2 390":
 *                   every 2nd frame is written as raw RGB565 (512x272) fr_NNNN.raw next to the EBOOT, the game exits
 *                   after max_frames. The script feeds a virtual pad through the same input layer as the real one.
 *
 * Controls (the controls design notes, scheme A default; SELECT+START switches to B): stick = walk / jog (camera-relative),
 * X tap = roll (backstep without stick), X hold = run, [] = light attack, L tap = lock-on on the demon if it is in
 * range and in front (else camera behind the knight), L tap again (stick centred) = unlock; locked: the knight faces
 * the target and strafes (MoveParam clips), rolls go in 4 directions relative to it. Scheme B: O tap/hold = roll/run,
 * R = light attack, L tap = lock / camera behind. HOME = exit.
 * Stdout "ash-game: ..." (PPSSPPHeadless log). Emulated timings are not hardware timings.
 */
#include <pspctrl.h>
#include <pspdebug.h>
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
#include "camera.h"
#include "chr.h"
#include "input.h"
#include "pcol.h"
#include "scene.h"

PSP_MODULE_INFO("Ash M4", 0, 0, 1);
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

/* ------------------------------------------------------------------ player */

typedef enum { ST_IDLE, ST_WALK, ST_JOG, ST_RUN, ST_ROLL, ST_BACKSTEP, ST_ATTACK, ST_COUNT } PState;
static const char *g_state_name[ST_COUNT] = {"idle", "walk", "jog", "run", "roll", "backstep", "attack"};
#define IS_ACTION(st) ((st) >= ST_ROLL)

enum { DIR_F, DIR_B, DIR_L, DIR_R, DIR_COUNT };
static const char *g_dir_sfx[DIR_COUNT] = {"", "_b", "_l", "_r"};

typedef struct {
    float pos[3], yaw;
    float vy;              /* vertical speed (falling) */
    int grounded, floor_tri;
    PState st;
    int dir;               /* clip direction relative to the facing (lock-on strafe / rolls) */
    int redirect;          /* root motion length applied along move_dir (lock-on strafe, side/back rolls) */
    float move_dir;
    int clip, prev_clip;
    float t, prev_t, blend;
    int buffered;          /* PState of a buffered action, -1 = none (the controls design notes §4.1: kept 0.25 s) */
    float buffer_left;
    /* §4.4: movement basis frozen at the start of a stick deflection */
    int basis_valid;
    float basis_yaw, basis_dir;
    float move_heading, speed_frac;
} Player;

typedef struct {
    AnimSet anim;
    Chr chr;
    int clip_of[ST_COUNT][DIR_COUNT];
    BonePose __attribute__((aligned(16))) pose_a[ANIM_MAX_BONES], pose_b[ANIM_MAX_BONES];
    ScePspFMatrix4 __attribute__((aligned(16))) world[ANIM_MAX_BONES], skin[ANIM_MAX_BONES];
} Knight;

static Knight g_k;
static const float BLEND_TIME = 5.0f / 30.0f;  /* 5 frames */
static const float FLOOR_Y = 182.05f;           /* stone floor of the hall (cook_scene "knight" pos) */
static const float BUFFER_TIME = 0.25f;

/* collision (.pcol) and the capsule of the player [версия: radius / step from DS1 feel, not from data] */
static Pcol g_col;
static int g_has_col;
static const PcolCapsule g_cap = {0.35f, 1.8f, 0.45f, 0.64f /* 50 deg */, 0.35f, 9.8f};

/* the Asylum Demon (c2230): lock-on target, idle animation */
typedef struct {
    int present;
    AnimSet anim;
    Chr chr;
    int idle, lock_bone;
    float pos[3], yaw, t, radius;
    float lock_pt[3];
    BonePose __attribute__((aligned(16))) pose[ANIM_MAX_BONES];
    ScePspFMatrix4 __attribute__((aligned(16))) world[ANIM_MAX_BONES], skin[ANIM_MAX_BONES];
} Demon;
static Demon g_d;

static float wrap_pi(float a)
{
    while (a > PI) a -= 2 * PI;
    while (a < -PI) a += 2 * PI;
    return a;
}

static void player_set_state(Player *p, PState st)
{
    p->prev_clip = p->clip;
    p->prev_t = p->t;
    p->clip = g_k.clip_of[st][p->dir];
    p->t = 0;
    p->blend = 0;
    p->st = st;
}

/* world move direction from the stick: basis = camera yaw frozen at deflection start (§4.4) */
static float stick_world(Player *p, const AshInput *in, float cam_yaw, float *mx, float *mz)
{
    if (in->move_mag <= 0) { p->basis_valid = 0; *mx = *mz = 0; return 0; }
    float dir = atan2f(in->move_x, -in->move_y);
    if (!p->basis_valid || fabsf(wrap_pi(dir - p->basis_dir)) > 45.0f * PI / 180.0f) {
        p->basis_valid = 1;
        p->basis_yaw = cam_yaw;
        p->basis_dir = dir;
    }
    float s = sinf(p->basis_yaw), c = cosf(p->basis_yaw);
    float n = sqrtf(in->move_x * in->move_x + in->move_y * in->move_y);
    float fx = -in->move_y / n, rx = in->move_x / n;
    /* camera forward (-s, -c), right (c, -s) */
    *mx = -s * fx + c * rx;
    *mz = -c * fx - s * rx;
    return in->move_mag;
}

static int dir_of(float local)
{
    float a = fabsf(local);
    if (a <= PI * 0.25f) return DIR_F;
    if (a >= PI * 0.75f) return DIR_B;
    return local > 0 ? DIR_L : DIR_R; /* yaw grows counter-clockwise from above: + = to the left */
}

/* lock: 1 = facing / strafing around `target` */
static void player_tick(Player *p, const AshInput *in, float cam_yaw, int lock, const float target[3], float dt)
{
    float mx, mz;
    float to_t = lock ? atan2f(-(target[0] - p->pos[0]), -(target[2] - p->pos[2])) : 0;
    /* locked: the stick is relative to the direction to the target (stick left = exact circle around it) */
    if (lock) { p->basis_valid = 0; cam_yaw = to_t; }
    float mag = stick_world(p, in, cam_yaw, &mx, &mz);
    float want_yaw = mag > 0 ? atan2f(-mx, -mz) : p->yaw;
    PState loco = mag <= 0 ? ST_IDLE : (in->held & ACT_RUN) ? ST_RUN : in->jog ? ST_JOG : ST_WALK;
    float face = lock ? to_t : want_yaw;
    if (lock && loco == ST_RUN) face = want_yaw; /* sprinting breaks the facing, as in DS1 */
    int ldir = (lock && (loco == ST_WALK || loco == ST_JOG)) ? dir_of(wrap_pi(want_yaw - face)) : DIR_F;

    int req = (in->pressed & ACT_DODGE) ? (mag > 0 ? ST_ROLL : ST_BACKSTEP) : (in->pressed & ACT_LIGHT) ? ST_ATTACK : -1;
    if (req >= 0) { p->buffered = req; p->buffer_left = BUFFER_TIME; }
    else if (p->buffer_left > 0 && (p->buffer_left -= dt) <= 0) p->buffered = -1;

    int busy = IS_ACTION(p->st);
    if (busy && p->t >= anim_duration(&g_k.anim, p->clip) - BLEND_TIME) { /* cancel window = end of the clip */
        busy = 0;
        if (p->buffered < 0) { p->dir = ldir; p->redirect = ldir != DIR_F; player_set_state(p, loco); }
    }
    if (!busy) {
        if (p->buffered >= 0) {
            p->dir = DIR_F;
            p->redirect = 0;
            if (p->buffered == ST_ROLL && mag > 0) {
                p->move_dir = want_yaw;
                float local = lock ? wrap_pi(want_yaw - face) : 0;
                if (!lock || fabsf(local) <= PI * 0.375f) p->yaw = want_yaw;     /* forward roll toward the stick */
                else { p->yaw = face; p->dir = fabsf(local) >= PI * 0.875f ? DIR_B : local > 0 ? DIR_L : DIR_R;
                       p->redirect = 1; }
            } else if (lock && p->buffered == ST_ATTACK) {
                p->yaw = face;
            }
            player_set_state(p, (PState)p->buffered);
            p->buffered = -1;
            p->buffer_left = 0;
        } else {
            if (loco != p->st || ldir != p->dir) { p->dir = ldir; player_set_state(p, loco); }
            p->redirect = ldir != DIR_F;
            p->move_dir = want_yaw;
            if (mag > 0 || lock) {
                float d = wrap_pi(face - p->yaw), step = 12.0f * dt; /* ~690 deg/s turn */
                p->yaw = wrap_pi(p->yaw + (d > step ? step : d < -step ? -step : d));
            }
        }
    }
    p->move_heading = mag > 0 ? want_yaw : p->yaw;
    p->speed_frac = p->st == ST_RUN ? 1.0f : p->st == ST_JOG ? 0.75f : p->st == ST_WALK ? 0.4f : 0.0f;

    /* root motion, weighted by the crossfade */
    float w = p->blend >= BLEND_TIME ? 1.0f : p->blend / BLEND_TIME;
    float d[4] = {0}, e[4];
    anim_root_delta(&g_k.anim, p->clip, p->t, p->t + dt, d);
    if (w < 1.0f && p->prev_clip >= 0) {
        anim_root_delta(&g_k.anim, p->prev_clip, p->prev_t, p->prev_t + dt, e);
        for (int i = 0; i < 4; i++) d[i] = d[i] * w + e[i] * (1 - w);
    }
    float s = sinf(p->yaw), c = cosf(p->yaw);
    float wx = c * d[0] + s * d[2], wz = -s * d[0] + c * d[2];
    if (p->redirect) { /* strafe / side roll: the clip's speed along the stick direction (smooth circling) */
        float len = sqrtf(wx * wx + wz * wz);
        wx = -sinf(p->move_dir) * len;
        wz = -cosf(p->move_dir) * len;
    }
    if (g_has_col) {
        pcol_move(&g_col, &g_cap, p->pos, wx, wz, &p->vy, dt, &p->floor_tri);
    } else {
        p->pos[0] += wx; p->pos[2] += wz; p->pos[1] = FLOOR_Y; p->floor_tri = -1;
    }
    if (g_d.present) { /* the demon's body: a vertical cylinder [версия: radius by eye] */
        float dx = p->pos[0] - g_d.pos[0], dz = p->pos[2] - g_d.pos[2], r = g_d.radius + g_cap.radius;
        float l = sqrtf(dx * dx + dz * dz);
        if (l < r && l > 1e-3f) { p->pos[0] = g_d.pos[0] + dx / l * r; p->pos[2] = g_d.pos[2] + dz / l * r; }
    }
    p->yaw = wrap_pi(p->yaw + d[3]);
    p->t += dt;
    p->prev_t += dt;
    p->blend += dt;
}

static void player_pose(const Player *p)
{
    float w = p->blend >= BLEND_TIME ? 1.0f : p->blend / BLEND_TIME;
    anim_sample(&g_k.anim, p->clip, p->t, g_k.pose_a);
    if (w < 1.0f && p->prev_clip >= 0) {
        anim_sample(&g_k.anim, p->prev_clip, p->prev_t, g_k.pose_b);
        anim_blend(g_k.anim.n_bones, g_k.pose_b, g_k.pose_a, w, g_k.pose_a);
    }
    anim_skin_matrices(g_k.anim.n_bones, g_k.chr.parent, g_k.pose_a, g_k.chr.inv_bind, g_k.world, g_k.skin);
}

/* ------------------------------------------------------------------ demon */

/* model-space point -> world: T(pos) * Ry(yaw), as sceGumRotateY */
static void demon_to_world(const float m[3], float out[3])
{
    float s = sinf(g_d.yaw), c = cosf(g_d.yaw);
    out[0] = g_d.pos[0] + c * m[0] + s * m[2];
    out[1] = g_d.pos[1] + m[1];
    out[2] = g_d.pos[2] - s * m[0] + c * m[2];
}

static void demon_tick(float dt)
{
    if (!g_d.present) return;
    g_d.t += dt;
    anim_sample(&g_d.anim, g_d.idle, g_d.t, g_d.pose);
    anim_skin_matrices(g_d.anim.n_bones, g_d.chr.parent, g_d.pose, g_d.chr.inv_bind, g_d.world, g_d.skin);
    float m[3] = {g_d.world[g_d.lock_bone].w.x, g_d.world[g_d.lock_bone].w.y, g_d.world[g_d.lock_bone].w.z};
    demon_to_world(m, g_d.lock_pt);
}

/* ------------------------------------------------------------------ lock-on (the controls design notes §4.6) */

static const float LOCK_CONE = 50.0f * PI / 180.0f, LOCK_NEAR_ANY = 6.0f, LOCK_BREAK_MUL = 1.35f;

static float flat_dist(const float a[3], const float b[3])
{
    float dx = a[0] - b[0], dz = a[2] - b[2];
    return sqrtf(dx * dx + dz * dz);
}

/* 1 if the demon can be taken: in radius, (in the cone of the camera or very close), visible from the camera */
static int lock_acquire(const Player *p, const CamState *cam, const CamRow *row)
{
    if (!g_d.present) return 0;
    float dist = flat_dist(p->pos, g_d.pos);
    if (dist > row->lock_radius) return 0;
    float fx = -sinf(cam->yaw), fz = -cosf(cam->yaw);
    float tx = g_d.pos[0] - p->pos[0], tz = g_d.pos[2] - p->pos[2], tl = sqrtf(tx * tx + tz * tz);
    float ang = tl > 1e-3f ? acosf(fminf(1.0f, fmaxf(-1.0f, (fx * tx + fz * tz) / tl))) : 0;
    if (ang > LOCK_CONE && dist > LOCK_NEAR_ANY) return 0;
    if (g_has_col) {
        float d[3] = {g_d.lock_pt[0] - cam->eye[0], g_d.lock_pt[1] - cam->eye[1], g_d.lock_pt[2] - cam->eye[2]}, t;
        if (pcol_raycast(&g_col, cam->eye, d, &t, -2.0f) >= 0) return 0; /* behind a wall: cannot take */
    }
    return 1;
}

static float cam_cast(void *ctx, const float a[3], const float b[3], float r)
{
    return pcol_sphere_cast((Pcol *)ctx, a, b, r);
}

/* ------------------------------------------------------------------ autopilot (headless has no pad) */

/* Raw pad script (scheme A), fed through the same input layer as the real pad. M4a scenario for the arena spawn
 * (scene.spawn of tools/cook_collision.py --scene arena: knight 17.5 m north of the Asylum Demon, facing it):
 * turn right, L = camera behind (the demon is not in front), run into the east wall (it stops the knight), turn the
 * back to the wall and L = camera behind -> the camera is pulled in against the wall, run diagonally toward the
 * demon, L = lock-on, strafe left around it, rolls right / left / back relative to it, L (stick centred) = unlock. */
static void demo_pad(int f, RawPad *p)
{
    p->buttons = 0;
    p->lx = p->ly = 128;
    if (f < 40) return;                                             /* idle (+ stick centre calibration) */
    if (f < 48) { p->lx = 128 + 50; return; }                       /* walk right: turn to the side wall */
    if (f == 60) { p->buttons = PSP_CTRL_LTRIGGER; return; }        /* L: no target in front -> camera behind */
    if (f >= 75 && f < 180) { p->ly = 0; if (f >= 87) p->buttons = PSP_CTRL_CROSS; return; } /* run into the wall */
    if (f >= 195 && f < 203) { p->ly = 128 + 50; return; }          /* turn the back to the wall */
    if (f == 215) { p->buttons = PSP_CTRL_LTRIGGER; return; }       /* L: camera behind = into the wall */
    if (f >= 285 && f < 370) { p->lx = 128 + 110; p->ly = 128 - 60; if (f >= 297) p->buttons = PSP_CTRL_CROSS;
                               return; }                            /* run diagonally to the demon */
    if (f == 420) { p->buttons = PSP_CTRL_LTRIGGER; return; }       /* L: lock-on */
    if (f >= 440 && f < 520) { p->lx = 0; return; }                 /* strafe left (jog) around the demon */
    if (f >= 540 && f < 545) { p->lx = 255; if (f < 542) p->buttons = PSP_CTRL_CROSS; return; } /* roll right */
    if (f >= 610 && f < 615) { p->lx = 0; if (f < 612) p->buttons = PSP_CTRL_CROSS; return; }   /* roll left */
    if (f >= 680 && f < 685) { p->ly = 255; if (f < 682) p->buttons = PSP_CTRL_CROSS; return; } /* roll back */
    if (f == 760) { p->buttons = PSP_CTRL_LTRIGGER; return; }       /* L, stick centred: unlock */
}

/* ------------------------------------------------------------------ timing */

static u32 g_tick_res;
static u64 now_ticks(void) { u64 t; sceRtcGetCurrentTick(&t); return t; }
static float us_since(u64 t0) { return (float)(now_ticks() - t0) * 1e6f / (float)g_tick_res; }

static void bench_anim(void)
{
    const int N = 200;
    int run = g_k.clip_of[ST_RUN][DIR_F], walk = g_k.clip_of[ST_WALK][DIR_F];
    for (int mode = 1; mode >= 0; mode--) {
        anim_set_vfpu(mode);
        u64 t0 = now_ticks();
        for (int i = 0; i < N; i++) {
            anim_sample(&g_k.anim, run, i * DT, g_k.pose_a);
            anim_sample(&g_k.anim, walk, i * DT, g_k.pose_b);
            anim_blend(g_k.anim.n_bones, g_k.pose_b, g_k.pose_a, 0.5f, g_k.pose_a);
            anim_skin_matrices(g_k.anim.n_bones, g_k.chr.parent, g_k.pose_a, g_k.chr.inv_bind, g_k.world, g_k.skin);
        }
        float us = us_since(t0) / N;
        u64 t1 = now_ticks();
        for (int i = 0; i < N; i++)
            anim_skin_matrices(g_k.anim.n_bones, g_k.chr.parent, g_k.pose_a, g_k.chr.inv_bind, g_k.world, g_k.skin);
        float us_m = us_since(t1) / N;
        printf("ash-game: BENCH anim %s: 2 clips sampled + blend + %d skin matrices = %.1f us/call "
               "(matrices only %.1f us) [emulated]\n", mode ? "VFPU" : "C", g_k.anim.n_bones, us, us_m);
    }
    anim_set_vfpu(1);
}

/* ------------------------------------------------------------------ main */

static void dump_frame(void *draw, int n)
{
    char name[64];
    snprintf(name, sizeof(name), "fr_%04d.raw", n);
    SceUID fd = sceIoOpen(name, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0) { printf("ash-game: cannot write %s (%x)\n", name, fd); return; }
    sceIoWrite(fd, (void *)(0x44000000 | (u32)draw), BUF_WIDTH * SCR_HEIGHT * 2);
    sceIoClose(fd);
}

/* small lock-on dot at the projected target point (2D sprites, after the 3D pass) */
typedef struct { u32 color; s16 x, y, z, pad; } MarkVert;

static void draw_lock_marker(const ScePspFMatrix4 *vp, const float p[3])
{
    float cx = vp->x.x * p[0] + vp->y.x * p[1] + vp->z.x * p[2] + vp->w.x;
    float cy = vp->x.y * p[0] + vp->y.y * p[1] + vp->z.y * p[2] + vp->w.y;
    float cw = vp->x.w * p[0] + vp->y.w * p[1] + vp->z.w * p[2] + vp->w.w;
    if (cw < 0.1f) return;
    int sx = (int)((cx / cw * 0.5f + 0.5f) * SCR_WIDTH), sy = (int)((0.5f - cy / cw * 0.5f) * SCR_HEIGHT);
    if (sx < 0 || sy < 0 || sx >= SCR_WIDTH || sy >= SCR_HEIGHT) return;
    MarkVert *v = sceGuGetMemory(4 * sizeof(MarkVert));
    v[0].color = 0xff202020; v[0].x = sx - 4; v[0].y = sy - 4; v[0].z = 0;
    v[1].color = 0xff202020; v[1].x = sx + 5; v[1].y = sy + 5; v[1].z = 0;
    v[2].color = 0xffe8f4ff; v[2].x = sx - 2; v[2].y = sy - 2; v[2].z = 0;
    v[3].color = 0xffe8f4ff; v[3].x = sx + 3; v[3].y = sy + 3; v[3].z = 0;
    sceGuDisable(GU_TEXTURE_2D);
    sceGuDisable(GU_DEPTH_TEST);
    sceGuDrawArray(GU_SPRITES, GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D, 4, 0, v);
    sceGuEnable(GU_DEPTH_TEST);
    sceGuEnable(GU_TEXTURE_2D);
}

static int load_all(Scene *sc)
{
    u8 *f;
    char path[256];
    int n = ash_load_file("scene.ashm", &f, path, sizeof(path));
    if (n < 0 || scene_load(sc, f, n) < 0) { printf("ash-game: scene.ashm missing/bad\n"); return -1; }
    printf("ash-game: %s %d B, %u tris\n", path, n, (unsigned)(sc->hdr->n_idx / 3));
    n = ash_load_file("knight.ashc", &f, path, sizeof(path));
    if (n < 0 || chr_load(&g_k.chr, f, n) < 0) { printf("ash-game: knight.ashc missing/bad\n"); return -1; }
    printf("ash-game: %s %d B, bones=%u chunks=%u verts=%u tris=%u\n", path, n, (unsigned)g_k.chr.hdr->n_bone,
           (unsigned)g_k.chr.hdr->n_chunk, (unsigned)g_k.chr.hdr->n_vert, (unsigned)(g_k.chr.hdr->n_idx / 3));
    n = ash_load_file("knight.ashanim", &f, path, sizeof(path));
    if (n < 0 || anim_load(&g_k.anim, f, n) < 0) { printf("ash-game: knight.ashanim missing/bad\n"); return -1; }
    if (g_k.anim.n_bones != (int)g_k.chr.hdr->n_bone) { printf("ash-game: bone count mismatch\n"); return -1; }
    const char *const *names = g_state_name;
    for (int i = 0; i < ST_COUNT; i++) {
        for (int d = 0; d < DIR_COUNT; d++) {
            char nm[16];
            snprintf(nm, sizeof(nm), "%s%s", names[i], g_dir_sfx[d]);
            int c = anim_find(&g_k.anim, nm);
            if (d == DIR_F && c < 0) { printf("ash-game: clip %s missing\n", nm); return -1; }
            g_k.clip_of[i][d] = c >= 0 ? c : g_k.clip_of[i][DIR_F];
            if (c < 0) continue;
            const AnimClip *ac = &g_k.anim.clips[c];
            printf("ash-game: clip %-8s id=%u frames=%u loop=%u root=%.2fm\n", nm, (unsigned)ac->anim_id,
                   ac->n_frames, ac->flags & 1, ac->root_dist);
        }
    }
    printf("ash-game: %s %d B\n", path, n);
    n = ash_load_file("scene.pcol", &f, path, sizeof(path));
    if (n > 0 && pcol_load(&g_col, f, n) == 0) {
        g_has_col = 1;
        printf("ash-game: %s %d B, %u tris, %u verts, %u parts, grid %ux%u of %.1f m, list %u\n", path, n,
               (unsigned)g_col.hdr->n_tri, (unsigned)g_col.hdr->n_vert, (unsigned)g_col.hdr->n_part,
               (unsigned)g_col.hdr->nx, (unsigned)g_col.hdr->nz, g_col.hdr->cell, (unsigned)g_col.hdr->n_list);
    } else {
        printf("ash-game: no scene.pcol, flat floor y=%.2f, no walls\n", FLOOR_Y);
    }
    n = ash_load_file("demon.ashc", &f, path, sizeof(path));
    if (n > 0 && chr_load(&g_d.chr, f, n) == 0) {
        u8 *fa;
        int na = ash_load_file("demon.ashanim", &fa, NULL, 0);
        if (na > 0 && anim_load(&g_d.anim, fa, na) == 0 && g_d.anim.n_bones == (int)g_d.chr.hdr->n_bone &&
            (g_d.idle = anim_find(&g_d.anim, "idle")) >= 0) {
            g_d.present = 1;
            g_d.lock_bone = chr_bone(&g_d.chr, "Spine1");
            if (g_d.lock_bone < 0) g_d.lock_bone = 0;
            printf("ash-game: %s %d B, bones=%u chunks=%u verts=%u tris=%u, idle frames=%u, lock bone %d\n", path, n,
                   (unsigned)g_d.chr.hdr->n_bone, (unsigned)g_d.chr.hdr->n_chunk, (unsigned)g_d.chr.hdr->n_vert,
                   (unsigned)(g_d.chr.hdr->n_idx / 3), g_d.anim.clips[g_d.idle].n_frames, g_d.lock_bone);
        }
    }
    if (!g_d.present) printf("ash-game: no demon (demon.ashc / demon.ashanim)\n");
    return 0;
}

int main(void)
{
    int th = sceKernelCreateThread("cb_thread", cb_thread, 0x11, 0xFA0, 0, 0);
    if (th >= 0) sceKernelStartThread(th, 0, 0);
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);
    g_tick_res = sceRtcGetTickResolution();

    Scene sc;
    if (load_all(&sc) < 0) { sceKernelExitGame(); return 0; }
    printf("ash-game: loaded, free=%u KB max block=%u KB\n", (unsigned)(sceKernelTotalFreeMemSize() / 1024),
           (unsigned)(sceKernelMaxFreeMemSize() / 1024));

    int demo = 0, dump_every = 0, demo_frames = 0;
    {
        u8 *buf = NULL;
        if (ash_load_file("demo.txt", &buf, NULL, 0) > 0) {
            demo = 1;
            sscanf((const char *)buf, "%d %d", &dump_every, &demo_frames);
            free(buf);
        }
    }
    float vd = anim_selftest_vfpu();
    printf("ash-game: VFPU selftest max diff %.2g -> %s\n", vd, anim_vfpu() ? "VFPU" : "C fallback");
    bench_anim();

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
    /* character light: 1 directional + ambient (PLAN §3.6); the map has its light baked */
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

    Player pl;
    memset(&pl, 0, sizeof(pl));
    /* spawn: player spot + target from scene.spawn (cook_collision.py, MSB); defaults = the pit (c2230) */
    pl.pos[0] = -3.0f; pl.pos[1] = FLOOR_Y; pl.pos[2] = -24.0f;
    pl.yaw = PI;
    g_d.pos[0] = -3.31f; g_d.pos[1] = 182.11f; g_d.pos[2] = -9.46f; g_d.yaw = 0;
    {
        u8 *buf = NULL;
        char model[16] = "";
        if (ash_load_file("scene.spawn", &buf, NULL, 0) > 0) {
            const char *l2 = strstr((const char *)buf, "target");
            sscanf((const char *)buf, "player %f %f %f %f", &pl.pos[0], &pl.pos[1], &pl.pos[2], &pl.yaw);
            if (l2) sscanf(l2, "target %15s %f %f %f %f", model, &g_d.pos[0], &g_d.pos[1], &g_d.pos[2], &g_d.yaw);
            printf("ash-game: spawn player (%.2f %.2f %.2f) target %s (%.2f %.2f %.2f) yaw %.0f\n", pl.pos[0],
                   pl.pos[1], pl.pos[2], model, g_d.pos[0], g_d.pos[1], g_d.pos[2], g_d.yaw * 180 / PI);
            free(buf);
        }
    }
    pl.clip = g_k.clip_of[ST_IDLE][DIR_F]; pl.prev_clip = -1; pl.st = ST_IDLE; pl.blend = 1; pl.buffered = -1;
    pl.floor_tri = -1;
    if (g_has_col) {
        float h;
        pl.floor_tri = pcol_floor(&g_col, pl.pos[0], pl.pos[2], pl.pos[1] + 2, pl.pos[1] - 5, g_cap.slope_cos, &h);
        if (pl.floor_tri >= 0) pl.pos[1] = h;
    }
    g_d.radius = 2.0f;
    if (g_has_col) pcol_floor(&g_col, g_d.pos[0], g_d.pos[2], g_d.pos[1] + 2, g_d.pos[1] - 5, 0.64f, &g_d.pos[1]);
    demon_tick(0);
    int locked = 0;
    CamCfg ccfg = cam_default_cfg();
    CamState cam;
    const CamRow *row = cam_row(g_has_col ? pcol_cam_row(&g_col, pl.floor_tri) : 0);
    cam_reset(&cam, &ccfg, row, pl.pos, pl.yaw);

    InputState inp;
    input_init(&inp, 0);
    void *draw = fbp0;
    SceCtrlData pad;
    const u32 fog_color = 0xff10141a;
    unsigned int frame = 0, frames_in_sec = 0, sim_frame = 0, dumps = 0;
    float fps = 0, a_us = 0, cpu_ms = 0, ge_ms = 0, col_us = 0, col_us_max = 0;
    double sum_anim = 0, sum_cpu = 0, sum_ge = 0, sum_col = 0;
    int col_tests = 0;
    unsigned int sim_total = 0;
    u64 t_prev = now_ticks(), t_last = t_prev;
    float acc = 0;

    while (g_running) {
        /* ---- simulation: fixed 1/30 s steps (autopilot: exactly one per frame, deterministic) */
        u64 tnow = now_ticks();
        acc += (float)(tnow - t_last) / (float)g_tick_res;
        t_last = tnow;
        int steps = demo ? 1 : (int)(acc / DT);
        if (steps > 2) steps = 2;
        if (!demo) acc -= steps * DT; else acc = 0;
        if (acc > DT) acc = DT;
        for (int s = 0; s < steps; s++) {
            RawPad raw;
            if (demo) {
                demo_pad((int)sim_frame, &raw);
            } else {
                sceCtrlPeekBufferPositive(&pad, 1);
                raw.buttons = pad.Buttons; raw.lx = pad.Lx; raw.ly = pad.Ly;
            }
            AshInput in;
            input_update(&inp, &raw, DT, &in);
            if (in.pressed & ACT_SCHEME) printf("ash-game: control scheme %s\n", input_scheme_name(&inp));
            /* lock-on: L tap (the controls design notes §3.1 A, §4.6-4.8) */
            if (in.pressed & ACT_LOCK) {
                if (locked) {
                    if (fabsf(in.move_x) < 0.6f) { locked = 0; printf("ash-game: f%u unlock\n", sim_frame); }
                    /* stick sideways = switch target: only one target in the arena -> the lock stays */
                } else if (lock_acquire(&pl, &cam, row)) {
                    locked = 1;
                    printf("ash-game: f%u lock-on, dist %.1f m, lock point (%.2f %.2f %.2f)\n", sim_frame,
                           flat_dist(pl.pos, g_d.pos), g_d.lock_pt[0], g_d.lock_pt[1], g_d.lock_pt[2]);
                } else {
                    printf("ash-game: f%u L: no target in front -> camera behind\n", sim_frame);
                }
            }
            if (locked && flat_dist(pl.pos, g_d.pos) > row->lock_radius * LOCK_BREAK_MUL) {
                locked = 0;
                printf("ash-game: f%u lock broken (distance)\n", sim_frame);
            }
            u64 tc0 = now_ticks();
            g_col.tests = 0;
            player_tick(&pl, &in, cam.yaw, locked, g_d.pos, DT);
            demon_tick(DT);
            if (g_has_col && pl.floor_tri >= 0) row = cam_row(pcol_cam_row(&g_col, pl.floor_tri));
            CamInput ci;
            ci.char_pos[0] = pl.pos[0]; ci.char_pos[1] = pl.pos[1]; ci.char_pos[2] = pl.pos[2];
            ci.char_facing = pl.yaw;
            ci.move_heading = pl.move_heading;
            ci.speed_frac = pl.speed_frac;
            ci.locomotion = pl.st == ST_WALK || pl.st == ST_JOG || pl.st == ST_RUN;
            ci.in_action = IS_ACTION(pl.st);
            ci.reset = !locked && (in.pressed & (ACT_LOCK | ACT_CAM_RESET)) != 0;
            ci.row = row;
            ci.locked = locked;
            ci.target[0] = g_d.lock_pt[0]; ci.target[1] = g_d.lock_pt[1]; ci.target[2] = g_d.lock_pt[2];
            ci.target_radius = 2.5f; /* [версия] the demon's size for framing */
            ci.cast = g_has_col ? cam_cast : NULL;
            ci.cast_ctx = &g_col;
            ci.yaw_in = in.cam_yaw_in;
            ci.pitch_in = in.cam_pitch_in;
            cam_update(&cam, &ccfg, &ci, DT);
            float cus = us_since(tc0);
            col_us = col_us * 0.9f + 0.1f * cus;
            if (cus > col_us_max) col_us_max = cus;
            sum_col += cus;
            col_tests = g_col.tests;
            sim_total++;
            sim_frame++;
        }

        u64 t0 = now_ticks();
        player_pose(&pl);
        float anim_us = us_since(t0);

        /* ---- draw */
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
        sceGumPerspective(cam.row_fov, 480.0f / 272.0f, 0.3f, 400.0f);
        sceGumStoreMatrix(&proj);
        sceGumMatrixMode(GU_VIEW);
        sceGumLoadIdentity();
        {
            ScePspFVector3 eye = {cam.eye[0], cam.eye[1], cam.eye[2]};
            ScePspFVector3 at = {cam.at[0], cam.at[1], cam.at[2]};
            ScePspFVector3 up = {0, 1, 0};
            sceGumLookAt(&eye, &at, &up);
        }
        sceGumStoreMatrix(&view);
        ash_mat_mul(&vp, &proj, &view);
        sceGumMatrixMode(GU_MODEL);
        sceGumLoadIdentity();
        int nb = 0;
        int tris = scene_draw(&sc, &vp, &nb);

        /* knight: model = T(pos) * Ry(yaw); bones = skin matrices; GE lighting on */
        sceGumMatrixMode(GU_MODEL);
        sceGumLoadIdentity();
        {
            ScePspFVector3 t = {pl.pos[0], pl.pos[1], pl.pos[2]};
            sceGumTranslate(&t);
            sceGumRotateY(pl.yaw);
        }
        sceGuEnable(GU_LIGHTING);
        sceGuColor(0xffffffff);
        sceGuAmbientColor(0xffffffff);
        tris += chr_draw(&g_k.chr, g_k.skin);
        if (g_d.present) {
            sceGumMatrixMode(GU_MODEL);
            sceGumLoadIdentity();
            ScePspFVector3 t = {g_d.pos[0], g_d.pos[1], g_d.pos[2]};
            sceGumTranslate(&t);
            sceGumRotateY(g_d.yaw);
            tris += chr_draw(&g_d.chr, g_d.skin);
        }
        sceGuDisable(GU_LIGHTING);
        sceGuDisable(GU_FOG);
        if (locked) draw_lock_marker(&vp, g_d.lock_pt);
        sceGuFinish();
        float list_ms = us_since(t0) / 1000.0f;
        u64 t1 = now_ticks();
        sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);
        float sync_ms = us_since(t1) / 1000.0f;

        a_us = a_us * 0.9f + 0.1f * anim_us;
        cpu_ms = cpu_ms * 0.9f + 0.1f * list_ms;
        ge_ms = ge_ms * 0.9f + 0.1f * sync_ms;
        sum_anim += anim_us; sum_cpu += list_ms; sum_ge += sync_ms;

        pspDebugScreenSetBase((u32 *)(0x44000000 | (u32)draw));
        pspDebugScreenSetXY(0, 0);
        pspDebugScreenSetTextColor(0xff9adfff);
        pspDebugScreenPrintf("ASH M4 %4.1ffps %-8s anim %3.0fus col %4.0fus %u tri [%s]%s", fps, g_state_name[pl.st],
                             a_us, col_us, tris, input_scheme_name(&inp), locked ? " LOCK" : "");
        if (demo) {
            pspDebugScreenSetXY(0, 1);
            pspDebugScreenPrintf("autopilot f%u", sim_frame);
        }
        if (demo && dump_every > 0 && sim_frame % dump_every == 0) dump_frame(draw, dumps++);

        sceDisplayWaitVblankStart();
        draw = sceGuSwapBuffers();
        frame++;
        frames_in_sec++;
        u64 tn = now_ticks();
        if (tn - t_prev >= g_tick_res) {
            fps = (float)frames_in_sec * (float)g_tick_res / (float)(tn - t_prev);
            printf("ash-game: frame=%u sim=%u fps=%.1f st=%s%s pos=(%.2f %.2f %.2f) yaw=%.0f cam=%.0f camdist=%.2f/%.2f "
                   "lock=%d row=%d anim=%.0fus col=%.0fus(max %.0f, %d tests) cpu_list=%.2fms ge_wait=%.2fms tris=%d "
                   "[emulated]\n", frame, sim_frame, fps, g_state_name[pl.st], g_dir_sfx[pl.dir], pl.pos[0],
                   pl.pos[1], pl.pos[2], pl.yaw * 180 / PI, cam.yaw * 180 / PI, cam.cur_dist,
                   cam.row_dist + cam.frame_extra, locked, row->id, a_us, col_us, col_us_max, col_tests, cpu_ms,
                   ge_ms, tris);
            col_us_max = 0;
            frames_in_sec = 0;
            t_prev = tn;
        }
        if (demo && demo_frames > 0 && (int)sim_frame >= demo_frames) {
            printf("ash-game: autopilot done: %u frames, %u dumps, avg anim=%.1fus col=%.1fus cpu_list=%.2fms "
                   "ge_wait=%.2fms [emulated]\n", frame, dumps, sum_anim / frame, sum_col / (sim_total ? sim_total : 1),
                   sum_cpu / frame, sum_ge / frame);
            break;
        }
    }
    printf("ash-game: exit after %u frames\n", frame);
    sceGuTerm();
    sceKernelExitGame();
    return 0;
}
