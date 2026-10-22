/*
 * Firelink Shrine test EBOOT (docs/firelink.md): the trimmed m10_02 zone pak (tools/cook_firelink.py) drawn with
 * src/render mips + zone fog, the Elite Knight at the bonfire (idle) with bonfire fire particles, the Crestfallen
 * Warrior seated (frozen first frame of a00_7810), fixed stills and a scripted camera flight for the video.
 * World drawing = a copy of src/world world_draw with render_tex_bind (mip chains) and a separate fog for the far
 * ring; src/world, src/render, src/game are only compiled in, not changed.
 *
 * Files next to the EBOOT: zone.pak, zone.ashmip, zone.cfg (cook_firelink sidecars), knight.ashc/.ashanim,
 * crestfallen.ashc/.ashanim, fl.txt (script, see load_script).
 * Output: s_<name>.raw (stills) / v_NNNN.raw (video), RGB565 512x272; stdout "fl: ..." per dumped frame.
 */
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
#include "pcol.h"
#include "render.h"
#include "scene.h"
#include "world.h"

PSP_MODULE_INFO("Ash Firelink", 0, 0, 1);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);
PSP_HEAP_SIZE_KB(-1024);

#define BUF_WIDTH 512
#define SCR_WIDTH 480
#define SCR_HEIGHT 272
#define PI 3.14159265f

static unsigned int __attribute__((aligned(64))) g_list[256 * 1024];

typedef struct {
    AnimSet anim;
    Chr chr;
    int clip, frozen;
    float pos[3], yaw, t;
    BonePose __attribute__((aligned(16))) pose[ANIM_MAX_BONES];
    ScePspFMatrix4 __attribute__((aligned(16))) world[ANIM_MAX_BONES], skin[ANIM_MAX_BONES];
    int ok;
} Actor;

static World W;
static Pcol C;
static int g_has_col;
static RenderMips g_mips;
static int g_has_mips;
static Actor g_k, g_cw;
static float g_bonfire[3] = {51.8f, -60.6f, 55.7f};
static float g_start[4] = {47.41f, -59.82f, 53.3f, 0};
static float g_cwp[4] = {51.03f, -59.59f, 62.81f, -0.1745f};
/* fog [придумал на глаз, от FogBank 20 'B2基本(拠点)' цвет (95,95,92), DS1 end 70 м]: near world / far ring */
static float g_fog_n = 18.0f, g_fog_f = 78.0f, g_ffog_n = 60.0f, g_ffog_f = 460.0f;
static u32 g_fog_col = 0xff4c5050;

typedef struct { int tris, sky, far, world, obj, chr, batches, tex; } Stats;

static int floor_cb(void *ctx, float x, float z, float y_top, float y_bot, float *h)
{
    (void)ctx;
    return g_has_col && pcol_floor(&C, x, z, y_top, y_bot, 0.5f, h) >= 0;
}

static int load_actor(Actor *a, const char *c, const char *an, const char *clip)
{
    u8 *f;
    int n = ash_load_file(c, &f, NULL, 0);
    if (n < 0 || chr_load(&a->chr, f, n) < 0) return -1;
    n = ash_load_file(an, &f, NULL, 0);
    if (n < 0 || anim_load(&a->anim, f, n) < 0) return -1;
    a->clip = anim_find(&a->anim, clip);
    if (a->clip < 0) a->clip = 0;
    a->ok = 1;
    return 0;
}

static void pose(Actor *a)
{
    if (!a->ok) return;
    anim_sample(&a->anim, a->clip, a->frozen ? 0.0f : a->t, a->pose);
    anim_skin_matrices(a->anim.n_bones, a->chr.parent, a->pose, a->chr.inv_bind, a->world, a->skin);
}

/* ------------------------------------------------------------------ world (copy of world_draw + mips) */

static float g_pl[6][4];

static void planes(const ScePspFMatrix4 *m)
{
    float r[4][4];
    const float *a = (const float *)m;
    for (int row = 0; row < 4; row++)
        for (int col = 0; col < 4; col++) r[row][col] = a[col * 4 + row];
    for (int i = 0; i < 3; i++)
        for (int s = 0; s < 2; s++) {
            float *p = g_pl[i * 2 + s];
            float sg = s ? -1.0f : 1.0f;
            for (int k = 0; k < 4; k++) p[k] = r[3][k] + sg * r[i][k];
            float nn = sqrtf(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
            if (nn > 0) for (int k = 0; k < 4; k++) p[k] /= nn;
        }
}

static int vis(const float c[3], float rad, int n)
{
    for (int i = 0; i < n; i++)
        if (g_pl[i][0] * c[0] + g_pl[i][1] * c[1] + g_pl[i][2] * c[2] + g_pl[i][3] < -rad) return 0;
    return 1;
}

typedef struct { int tex, alpha, cull; } DS;

static void draw_batch(DS *ds, const WorldBatch *b, const float *R, const float *t, Stats *st, int *bucket)
{
    ScePspFMatrix4 m;
    const float *s = b->pos_scale, *o = b->pos_off;
    if (R) {
        m.x.x = R[0] * s[0]; m.x.y = R[3] * s[0]; m.x.z = R[6] * s[0]; m.x.w = 0;
        m.y.x = R[1] * s[1]; m.y.y = R[4] * s[1]; m.y.z = R[7] * s[1]; m.y.w = 0;
        m.z.x = R[2] * s[2]; m.z.y = R[5] * s[2]; m.z.z = R[8] * s[2]; m.z.w = 0;
        m.w.x = R[0] * o[0] + R[1] * o[1] + R[2] * o[2] + t[0];
        m.w.y = R[3] * o[0] + R[4] * o[1] + R[5] * o[2] + t[1];
        m.w.z = R[6] * o[0] + R[7] * o[1] + R[8] * o[2] + t[2];
        m.w.w = 1;
    } else {
        memset(&m, 0, sizeof(m));
        m.x.x = s[0]; m.y.y = s[1]; m.z.z = s[2];
        m.w.x = o[0]; m.w.y = o[1]; m.w.z = o[2]; m.w.w = 1;
    }
    sceGuSetMatrix(GU_MODEL, &m);
    if (b->tex != ds->tex) {
        const WorldTex *tx = &W.tex[b->tex];
        render_tex_bind(tx->psm, tx->w, tx->h, W.tclut[b->tex], W.tpix[b->tex], g_has_mips ? &g_mips : NULL, b->tex);
        ds->tex = b->tex;
        st->tex++;
    }
    int alpha = (b->flags & WB_ALPHA) ? 1 : 0;
    if (alpha != ds->alpha) { if (alpha) sceGuEnable(GU_ALPHA_TEST); else sceGuDisable(GU_ALPHA_TEST); ds->alpha = alpha; }
    int cull = (b->flags & WB_NOCULL) ? 0 : 1;
    if (cull != ds->cull) { if (cull) sceGuEnable(GU_CULL_FACE); else sceGuDisable(GU_CULL_FACE); ds->cull = cull; }
    sceGuTexScale(b->uv_scale[0], b->uv_scale[1]);
    sceGuTexOffset(b->uv_off[0], b->uv_off[1]);
    sceGuDrawArray(GU_TRIANGLES, GU_TEXTURE_16BIT | GU_COLOR_5650 | GU_VERTEX_16BIT | GU_INDEX_16BIT | GU_TRANSFORM_3D,
                   b->idx_count, W.idx + b->idx_start, W.vert + b->vert_start);
    st->tris += b->idx_count / 3;
    *bucket += b->idx_count / 3;
    st->batches++;
}

static void world_draw_fl(const ScePspFMatrix4 *proj, const ScePspFMatrix4 *view, const float eye[3], Stats *st)
{
    DS ds = {-1, -1, -1};
    ScePspFMatrix4 vp, sky = *proj;
    ash_mat_mul(&vp, proj, view);
    planes(&vp);
    sceGuSetMatrix(GU_VIEW, view);
    sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
    sceGuTexWrap(GU_REPEAT, GU_REPEAT);
    sceGuAlphaFunc(GU_GREATER, 0x80, 0xff);
    sceGuDisable(GU_LIGHTING);
    const int nb = (int)W.w->n_batch;
    /* 1. sky: own far plane, no depth, no fog */
    {
        const float n = 4.0f, f = 6500.0f;
        sky.z.z = (f + n) / (n - f);
        sky.w.z = 2.0f * f * n / (n - f);
        sceGuSetMatrix(GU_PROJECTION, &sky);
        sceGuDisable(GU_FOG);
        sceGuDisable(GU_DEPTH_TEST);
        sceGuDepthMask(GU_TRUE);
        for (int i = 0; i < nb; i++) {
            const WorldBatch *b = &W.batch[i];
            if (!(b->flags & WB_SKY) || !vis(b->center, b->radius, 4)) continue;
            draw_batch(&ds, b, NULL, NULL, st, &st->sky);
        }
        sceGuEnable(GU_DEPTH_TEST);
        sceGuDepthMask(GU_FALSE);
    }
    /* 2. far ring: own wide fog, far plane of the sky projection (300+ m), depth on */
    sceGuSetMatrix(GU_PROJECTION, &sky);
    sceGuFog(g_ffog_n, g_ffog_f, g_fog_col);
    sceGuEnable(GU_FOG);
    for (int i = 0; i < nb; i++) {
        const WorldBatch *b = &W.batch[i];
        if (!(b->flags & WB_FAR) || !vis(b->center, b->radius, 4)) continue;
        draw_batch(&ds, b, NULL, NULL, st, &st->far);
    }
    /* the near world goes over the far ring regardless of depth precision: clear Z only */
    sceGuClear(GU_DEPTH_BUFFER_BIT);
    sceGuSetMatrix(GU_PROJECTION, proj);
    sceGuFog(g_fog_n, g_fog_f, g_fog_col);
    /* 3. world, LOD by chunk, fog-distance cull */
    for (int i = 0; i < nb; i++) {
        const WorldBatch *b = &W.batch[i];
        if (b->flags & (WB_SKY | WB_FAR | WB_MESH)) continue;
        if (b->flags & (WB_LOD_NEAR | WB_LOD_FAR)) {
            float ch = W.w->chunk;
            float cx = ((int)(b->chunk & 1023) - 512 + 0.5f) * ch - eye[0];
            float cy = ((int)((b->chunk >> 10) & 1023) - 512 + 0.5f) * ch - eye[1];
            float cz = ((int)((b->chunk >> 20) & 1023) - 512 + 0.5f) * ch - eye[2];
            int near = cx * cx + cy * cy + cz * cz < W.lod_dist * W.lod_dist;
            if (near != ((b->flags & WB_LOD_NEAR) != 0)) continue;
        }
        float dx = b->center[0] - eye[0], dy = b->center[1] - eye[1], dz = b->center[2] - eye[2];
        float lim = ((b->flags & WB_DECOR) ? W.decor_dist : W.far_dist) + b->radius;
        if (dx * dx + dy * dy + dz * dz > lim * lim) continue;
        if (!vis(b->center, b->radius, 6)) continue;
        draw_batch(&ds, b, NULL, NULL, st, &st->world);
    }
    /* 4. objects (bonfire, lift, well cover) */
    for (u32 i = 0; i < W.z->n_obj; i++) {
        const ZoneObj *o = &W.obj[i];
        if (o->mesh < 0 || !W.obj_on[i]) continue;
        const WorldMesh *m = &W.mesh[o->mesh];
        const float *M = W.obj_m[i];
        if (!vis(M + 9, m->radius, 6)) continue;
        for (u32 k = 0; k < m->batch_count; k++) draw_batch(&ds, &W.batch[m->batch_start + k], M, M + 9, st, &st->obj);
    }
    sceGuDisable(GU_ALPHA_TEST);
    sceGuEnable(GU_CULL_FACE);
    sceGuTexScale(1.0f, 1.0f);
    sceGuTexOffset(0.0f, 0.0f);
    sceGuTexFilter(GU_LINEAR, GU_LINEAR);
    sceGuTexMode(GU_PSM_T8, 0, 0, 1);
    {
        ScePspFMatrix4 id;
        memset(&id, 0, sizeof(id));
        id.x.x = id.y.y = id.z.z = id.w.w = 1.0f;
        sceGuSetMatrix(GU_MODEL, &id);
    }
}

static void draw_actor(Actor *a, Stats *st)
{
    if (!a->ok) return;
    sceGumMatrixMode(GU_MODEL);
    sceGumLoadIdentity();
    ScePspFVector3 t = {a->pos[0], a->pos[1], a->pos[2]};
    sceGumTranslate(&t);
    sceGumRotateY(a->yaw);
    float chest[3] = {a->pos[0], a->pos[1] + 1.2f, a->pos[2]};
    render_char_light_begin(chest);
    int n = chr_draw(&a->chr, a->skin);
    render_char_light_end();
    st->chr += n;
    st->tris += n;
}

/* ------------------------------------------------------------------ frame */

typedef struct { float eye[3], at[3], fov; char name[24]; } Cam;

static void *g_draw;

static void dump(const char *name)
{
    SceUID fd = sceIoOpen(name, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0) { printf("fl: cannot write %s\n", name); return; }
    sceIoWrite(fd, (void *)(0x44000000 | (u32)g_draw), BUF_WIDTH * SCR_HEIGHT * 2);
    sceIoClose(fd);
}

static void frame(const Cam *c, float dt, const char *dump_name)
{
    Stats st;
    memset(&st, 0, sizeof(st));
    sceGuStart(GU_DIRECT, g_list);
    ScePspFMatrix4 proj, view;
    sceGumMatrixMode(GU_PROJECTION);
    sceGumLoadIdentity();
    sceGumPerspective(c->fov, 480.0f / 272.0f, 0.3f, 300.0f);
    sceGumStoreMatrix(&proj);
    sceGumMatrixMode(GU_VIEW);
    sceGumLoadIdentity();
    ScePspFVector3 eye = {c->eye[0], c->eye[1], c->eye[2]}, at = {c->at[0], c->at[1], c->at[2]}, up = {0, 1, 0};
    sceGumLookAt(&eye, &at, &up);
    sceGumStoreMatrix(&view);
    render_begin_frame(&view, &proj, c->eye, dt);
    sceGuClearDepth(0);
    sceGuClear(GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT);
    world_draw_fl(&proj, &view, c->eye, &st);
    sceGuSetMatrix(GU_PROJECTION, &proj);
    sceGumMatrixMode(GU_PROJECTION);
    sceGumLoadMatrix(&proj);
    sceGumMatrixMode(GU_VIEW);
    sceGumLoadMatrix(&view);
    render_fog_apply();
    draw_actor(&g_k, &st);
    draw_actor(&g_cw, &st);
    render_shadow_blob(g_k.pos, 0.55f, floor_cb, NULL);
    render_fx_draw();
    render_end_frame();
    sceGuFinish();
    sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);
    if (dump_name) {
        dump(dump_name);
        printf("fl: %s tris=%d world=%d far=%d sky=%d obj=%d chr=%d batches=%d tex=%d fx=%d eye=%.1f,%.1f,%.1f\n",
               dump_name, st.tris, st.world, st.far, st.sky, st.obj, st.chr, st.batches, st.tex,
               g_render.st.fx_sprites, c->eye[0], c->eye[1], c->eye[2]);
    }
    sceDisplayWaitVblankStart();
    g_draw = sceGuSwapBuffers();
}

/* keep the camera out of walls: sphere cast from the look-at point to the eye on the zone collision (as the game
   camera does); smoothed so a pillar does not make it jump */
static float g_cam_t = 1.0f;
static void cam_fix(Cam *c, const float a[3], int smooth_it)
{
    if (!g_has_col) return;
    float t = pcol_sphere_cast(&C, a, c->eye, 0.5f);
    if (smooth_it) { g_cam_t += (t - g_cam_t) * (t < g_cam_t ? 0.5f : 0.15f); t = g_cam_t; } else g_cam_t = t;
    for (int k = 0; k < 3; k++) c->eye[k] = a[k] + (c->eye[k] - a[k]) * t;
}

static void tick(float dt)
{
    g_k.t += dt;
    pose(&g_k);
    pose(&g_cw);
}

/* ------------------------------------------------------------------ script (fl.txt)
 *   still <name> ex ey ez ax ay az [fov]
 *   orbit <frames> cx cy cz radius height a0deg a1deg [look_dy]
 *   move <frames> ex0 ey0 ez0 ax0 ay0 az0 ex1 ey1 ez1 ax1 ay1 az1
 *   warm <frames>        (fx warm-up before the first dump)
 *   K / B / CW in coordinates: offsets from knight / bonfire / crestfallen are not parsed; absolute PSP space. */

static char *g_script;

static void smooth(float u, float *s) { *s = u * u * (3 - 2 * u); }

static void run_script(void)
{
    const float dt = 1.0f / 30.0f;
    char name[64];
    int vframe = 0;
    char *p = g_script;
    while (p && *p) {
        char *nl = strchr(p, '\n');
        if (nl) *nl = 0;
        char kind[16] = "";
        sscanf(p, "%15s", kind);
        if (!strcmp(kind, "warm")) {
            int n = 0;
            sscanf(p, "%*s %d", &n);
            Cam c = {{g_bonfire[0] + 5, g_bonfire[1] + 3, g_bonfire[2] + 5}, {g_bonfire[0], g_bonfire[1], g_bonfire[2]}, 60, "w"};
            for (int i = 0; i < n; i++) { tick(dt); frame(&c, dt, NULL); }
        } else if (!strcmp(kind, "still")) {
            Cam c;
            c.fov = 60;
            char nm[24];
            int k = sscanf(p, "%*s %23s %f %f %f %f %f %f %f", nm, &c.eye[0], &c.eye[1], &c.eye[2], &c.at[0], &c.at[1],
                           &c.at[2], &c.fov);
            if (k >= 7) {
                for (int i = 0; i < 3; i++) { tick(dt); frame(&c, dt, NULL); }   /* fx settle for this view */
                snprintf(name, sizeof(name), "s_%s.raw", nm);
                frame(&c, dt, name);
            }
        } else if (!strcmp(kind, "orbit")) {
            int n = 0;
            float cx, cy, cz, r, h, a0, a1, ldy = 0;
            sscanf(p, "%*s %d %f %f %f %f %f %f %f %f", &n, &cx, &cy, &cz, &r, &h, &a0, &a1, &ldy);
            for (int i = 0; i < n; i++) {
                float a = (a0 + (a1 - a0) * i / (float)n) * PI / 180.0f;
                Cam c = {{cx + sinf(a) * r, cy + h, cz + cosf(a) * r}, {cx, cy + ldy, cz}, 60, "orbit"};
                float anc[3] = {cx, cy + h * 0.6f, cz};   /* above the plaza floor */
                cam_fix(&c, anc, i > 0);
                tick(dt);
                snprintf(name, sizeof(name), "v_%04d.raw", vframe++);
                frame(&c, dt, name);
            }
        } else if (!strcmp(kind, "move")) {
            int n = 0;
            float A[6], Bv[6];
            sscanf(p, "%*s %d %f %f %f %f %f %f %f %f %f %f %f %f", &n, &A[0], &A[1], &A[2], &A[3], &A[4], &A[5],
                   &Bv[0], &Bv[1], &Bv[2], &Bv[3], &Bv[4], &Bv[5]);
            for (int i = 0; i < n; i++) {
                float u;
                smooth(i / (float)(n > 1 ? n - 1 : 1), &u);
                Cam c;
                for (int k = 0; k < 3; k++) {
                    c.eye[k] = A[k] + (Bv[k] - A[k]) * u;
                    c.at[k] = A[k + 3] + (Bv[k + 3] - A[k + 3]) * u;
                }
                c.fov = 60;
                tick(dt);
                snprintf(name, sizeof(name), "v_%04d.raw", vframe++);
                frame(&c, dt, name);
            }
        }
        p = nl ? nl + 1 : NULL;
    }
}

int main(void)
{
    u8 *pak;
    int n = world_read_file("zone.pak", &pak);
    if (n < 0 || world_load(&W, pak, n) != 0) { printf("fl: no zone.pak\n"); sceKernelExitGame(); return 0; }
    world_set_cell(&W, -1);
    W.far_dist = 82.0f;
    W.lod_dist = 16.0f;
    W.decor_dist = 30.0f;
    g_has_col = W.pcol && pcol_load(&C, W.pcol, W.pcol_size) == 0;
    printf("fl: zone %d B, %u tex, %u batches, %u verts, %u tris; pcol %s\n", n, (unsigned)W.w->n_tex,
           (unsigned)W.w->n_batch, (unsigned)W.w->n_vert, (unsigned)(W.w->n_idx / 3), g_has_col ? "yes" : "no");
    render_init();
    g_render.flags = RF_FOG | RF_MIPS | RF_CHARLIGHT | RF_SHADOWS | RF_FX;
    {
        u8 *f;
        int m = ash_load_file("zone.ashmip", &f, NULL, 0);
        g_has_mips = m > 0 && render_mips_load(&g_mips, f, m) == 0;
        printf("fl: mips %s (%d B)\n", g_has_mips ? "yes" : "no", m);
        if (ash_load_file("zone.cfg", &f, NULL, 0) > 0) {
            char *s = (char *)f, *l;
            if ((l = strstr(s, "bonfire "))) sscanf(l, "bonfire %f %f %f", &g_bonfire[0], &g_bonfire[1], &g_bonfire[2]);
            if ((l = strstr(s, "player "))) sscanf(l, "player %f %f %f %f", &g_start[0], &g_start[1], &g_start[2], &g_start[3]);
            if ((l = strstr(s, "chr c0000 6270 "))) sscanf(l, "chr c0000 6270 %f %f %f %f", &g_cwp[0], &g_cwp[1], &g_cwp[2], &g_cwp[3]);
            if ((l = strstr(s, "lod "))) sscanf(l, "lod %f %f", &W.lod_dist, &W.far_dist);
            if ((l = strstr(s, "look "))) sscanf(l, "look %f %f %f %f %x", &g_fog_n, &g_fog_f, &g_ffog_n, &g_ffog_f, (unsigned *)&g_fog_col);
        }
        if (ash_load_file("fl.txt", &f, NULL, 0) > 0) g_script = (char *)f;
    }
    g_render.fog.near = g_fog_n;
    g_render.fog.far = g_fog_f;
    g_render.fog.color = g_fog_col;
    g_render.fog.clear = g_fog_col;
    /* character light: overcast day, warm bonfire side [придумал] */
    g_render.chl.dir[0][0] = -0.3f; g_render.chl.dir[0][1] = 0.85f; g_render.chl.dir[0][2] = -0.4f;
    g_render.chl.col[0] = 0xff8a9aa8;
    g_render.chl.ambient = 0xff505050;

    if (load_actor(&g_k, "knight.ashc", "knight.ashanim", "idle") < 0) printf("fl: no knight\n");
    if (load_actor(&g_cw, "crestfallen.ashc", "crestfallen.ashanim", "sit") < 0) printf("fl: no crestfallen\n");
    g_cw.frozen = 1;
    /* knight 1.7 m from the bonfire towards the start region, facing the fire */
    float dx = g_start[0] - g_bonfire[0], dz = g_start[2] - g_bonfire[2], l = sqrtf(dx * dx + dz * dz) + 1e-4f;
    g_k.pos[0] = g_bonfire[0] + dx / l * 1.7f;
    g_k.pos[2] = g_bonfire[2] + dz / l * 1.7f;
    g_k.pos[1] = g_bonfire[1] + 1.0f;
    if (g_has_col) pcol_floor(&C, g_k.pos[0], g_k.pos[2], g_k.pos[1] + 1.5f, g_k.pos[1] - 4.0f, 0.5f, &g_k.pos[1]);
    g_k.yaw = atan2f(-dx, -dz);
    memcpy(g_cw.pos, g_cwp, 3 * sizeof(float));
    g_cw.yaw = g_cwp[3];
    printf("fl: bonfire %.2f %.2f %.2f knight %.2f %.2f %.2f crestfallen %.2f %.2f %.2f\n", g_bonfire[0], g_bonfire[1],
           g_bonfire[2], g_k.pos[0], g_k.pos[1], g_k.pos[2], g_cw.pos[0], g_cw.pos[1], g_cw.pos[2]);
    float fire[3] = {g_bonfire[0], g_bonfire[1] + 0.25f, g_bonfire[2]};
    fx_add_fire(fire, 0.5f, 1);

    void *fbp0 = guGetStaticVramBuffer(BUF_WIDTH, SCR_HEIGHT, GU_PSM_5650);
    void *fbp1 = guGetStaticVramBuffer(BUF_WIDTH, SCR_HEIGHT, GU_PSM_5650);
    void *zbp = guGetStaticVramBuffer(BUF_WIDTH, SCR_HEIGHT, GU_PSM_4444);
    {
        int used = 3 * BUF_WIDTH * SCR_HEIGHT * 2;          /* as src/game/asy_main.c */
        int got = world_upload_vram(&W, (u8 *)sceGeEdramGetAddr() + used, sceGeEdramGetSize() - used);
        printf("fl: textures in VRAM: %d B\n", got);
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
    sceGuFinish();
    sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);
    sceDisplayWaitVblankStart();
    sceGuDisplay(GU_TRUE);
    g_draw = fbp0;

    run_script();
    printf("fl: done\n");
    sceGuTerm();
    sceKernelExitGame();
    return 0;
}
