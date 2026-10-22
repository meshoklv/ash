/*
 * Ash render test ("ДО/ПОСЛЕ" of src/render): the cooked DS scene (.ashm) + Elite Knight + demon, fixed cameras,
 * each frame rendered with the old look (flags 0 = what src/game draws on 14.10) and with src/render effects on,
 * plus a coverage frame (passes in solid magenta) for the fill-cost numbers, then an orbit for the video.
 * A copy of the src/game drawing logic (scene_draw with render_tex_bind), src/game itself is not touched.
 *
 * Files next to the EBOOT: scene.ashm, scene.ashmip (tools/texmip.py), scene.ashl (tools/cook_light.py),
 * scene.pcol, scene.spawn, knight.ashc/.ashanim, demon.ashc/.ashanim; rtest.txt = "<orbit_frames> <warmup>".
 * Output: c<N>_{before,after,cover}.raw and v_{before,after}_NNNN.raw (RGB565 512x272), stdout "rtest: ...".
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
#include "render_chr.h"
#include "scene.h"

PSP_MODULE_INFO("Ash Render Test", 0, 0, 1);
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
    int clip;
    float pos[3], yaw, t;
    BonePose __attribute__((aligned(16))) pose[ANIM_MAX_BONES];
    ScePspFMatrix4 __attribute__((aligned(16))) world[ANIM_MAX_BONES], skin[ANIM_MAX_BONES];
    int ok;
} Actor;

static Scene g_sc0, g_sc_v, g_sc;
static int g_has_vlit;
static RenderMips g_mips;
static int g_has_mips;
static RenderZone g_zone;
static int g_has_zone;
static Pcol g_col;
static int g_has_col;
static Actor g_k, g_d;

static int floor_cb(void *ctx, float x, float z, float y_top, float y_bot, float *h)
{
    (void)ctx;
    if (!g_has_col) return 0;
    return pcol_floor(&g_col, x, z, y_top, y_bot, 0.5f, h) >= 0;
}

static int load_actor(Actor *a, const char *c, const char *an, const char *clip, const char *clip2)
{
    u8 *f;
    int n = ash_load_file(c, &f, NULL, 0);
    if (n < 0 || chr_load(&a->chr, f, n) < 0) return -1;
    n = ash_load_file(an, &f, NULL, 0);
    if (n < 0 || anim_load(&a->anim, f, n) < 0) return -1;
    a->clip = anim_find(&a->anim, clip);
    if (a->clip < 0 && clip2) a->clip = anim_find(&a->anim, clip2);
    if (a->clip < 0) a->clip = 0;
    a->ok = 1;
    return 0;
}

static void pose(Actor *a)
{
    anim_sample(&a->anim, a->clip, a->t, a->pose);
    anim_skin_matrices(a->anim.n_bones, a->chr.parent, a->pose, a->chr.inv_bind, a->world, a->skin);
}

static void draw_scene(const ScePspFMatrix4 *vp, int *tris)
{
    /* copy of src/game/scene.c scene_draw + render_tex_bind (mips) + fog cull */
    static float pl[6][4];
    {
        float r[4][4];
        const float *a = (const float *)vp;
        for (int row = 0; row < 4; row++)
            for (int col = 0; col < 4; col++) r[row][col] = a[col * 4 + row];
        for (int i = 0; i < 3; i++)
            for (int s = 0; s < 2; s++) {
                float *p = pl[i * 2 + s];
                float sg = s ? -1.0f : 1.0f;
                for (int k = 0; k < 4; k++) p[k] = r[3][k] + sg * r[i][k];
                float nn = sqrtf(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
                if (nn > 0) for (int k = 0; k < 4; k++) p[k] /= nn;
            }
    }
    int cur_tex = -1, cur_alpha = -1, cur_cull = -1;
    sceGumMatrixMode(GU_MODEL);
    sceGumLoadIdentity();
    for (u32 i = 0; i < g_sc.hdr->n_batch; i++) {
        const AshmBatch *b = &g_sc.batch[i];
        int vis = 1;
        for (int k = 0; k < 6 && vis; k++)
            if (pl[k][0] * b->cx + pl[k][1] * b->cy + pl[k][2] * b->cz + pl[k][3] < -b->radius) vis = 0;
        if (!vis || !render_fog_visible(b->cx, b->cy, b->cz, b->radius)) continue;
        if (b->tex != cur_tex) {
            const AshmTex *t = &g_sc.tex[b->tex];
            render_tex_bind(GU_PSM_T8, t->w, t->h, g_sc.file + t->off_clut, g_sc.file + t->off_pix,
                            g_has_mips ? &g_mips : NULL, b->tex);
            cur_tex = b->tex;
        }
        int alpha = b->flags & 1;
        if (alpha != cur_alpha) { if (alpha) sceGuEnable(GU_ALPHA_TEST); else sceGuDisable(GU_ALPHA_TEST); cur_alpha = alpha; }
        int cull = (b->flags & 2) ? 0 : 1;
        if (cull != cur_cull) { if (cull) sceGuEnable(GU_CULL_FACE); else sceGuDisable(GU_CULL_FACE); cur_cull = cull; }
        sceGumDrawArray(GU_TRIANGLES, GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_INDEX_16BIT |
                        GU_TRANSFORM_3D, b->idx_count, g_sc.idx + b->idx_start, g_sc.vert + b->vert_start);
        *tris += b->idx_count / 3;
    }
    sceGuDisable(GU_ALPHA_TEST);
    sceGuEnable(GU_CULL_FACE);
    sceGuTexFilter(GU_LINEAR, GU_LINEAR);
    if (g_has_zone) render_light_pass_ashm(g_sc.vert, g_sc.batch, sizeof(AshmBatch));
}

static void draw_actor(Actor *a, int *tris)
{
    if (!a->ok) return;
    sceGumMatrixMode(GU_MODEL);
    sceGumLoadIdentity();
    ScePspFVector3 t = {a->pos[0], a->pos[1], a->pos[2]};
    sceGumTranslate(&t);
    sceGumRotateY(a->yaw);
    float chest[3] = {a->pos[0], a->pos[1] + 1.2f, a->pos[2]};
    render_char_light_begin(chest);
    *tris += chr_draw(&a->chr, a->skin);
    render_char_light_end();
    *tris += render_char_rim(&a->chr, a->skin, 1);
}

typedef struct { float eye[3], at[3], fov; const char *name; } Cam;

static void dump(void *draw, const char *name)
{
    SceUID fd = sceIoOpen(name, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0) { printf("rtest: cannot write %s\n", name); return; }
    sceIoWrite(fd, (void *)(0x44000000 | (u32)draw), BUF_WIDTH * SCR_HEIGHT * 2);
    sceIoClose(fd);
}

static void *g_draw;

/* one frame; returns tris */
static int frame(const Cam *c, u32 flags, float dt, const char *dump_name)
{
    g_render.flags = flags;
    g_sc = ((flags & RF_VLIT) && g_has_vlit) ? g_sc_v : g_sc0;
    sceGuStart(GU_DIRECT, g_list);
    ScePspFMatrix4 proj, view, vp;
    sceGumMatrixMode(GU_PROJECTION);
    sceGumLoadIdentity();
    sceGumPerspective(c->fov, 480.0f / 272.0f, 0.3f, 400.0f);
    sceGumStoreMatrix(&proj);
    sceGumMatrixMode(GU_VIEW);
    sceGumLoadIdentity();
    ScePspFVector3 eye = {c->eye[0], c->eye[1], c->eye[2]}, at = {c->at[0], c->at[1], c->at[2]}, up = {0, 1, 0};
    sceGumLookAt(&eye, &at, &up);
    sceGumStoreMatrix(&view);
    ash_mat_mul(&vp, &proj, &view);
    render_begin_frame(&view, &proj, c->eye, dt);
    sceGuClearDepth(0);
    sceGuClear(GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT);
    sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
    sceGuTexWrap(GU_REPEAT, GU_REPEAT);
    sceGuTexScale(1.0f, 1.0f);
    sceGuTexOffset(0.0f, 0.0f);
    sceGuAlphaFunc(GU_GREATER, 0x80, 0xff);
    int tris = 0;
    draw_scene(&vp, &tris);
    draw_actor(&g_k, &tris);
    draw_actor(&g_d, &tris);
    render_shadow_blob(g_k.pos, 0.55f, floor_cb, NULL);
    if (g_d.ok) render_shadow_blob(g_d.pos, 2.2f, floor_cb, NULL);
    render_fx_draw();
    render_end_frame();
    sceGuFinish();
    sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);
    if (dump_name) {
        dump(g_draw, dump_name);
        printf("rtest: %s flags=%x tris=%d light_tris=%d light_draws=%d rim_tris=%d shadow_tris=%d fx_sprites=%d "
               "fx_px=%d shadow_px=%d\n", dump_name, (unsigned)flags, tris, g_render.st.light_tris, g_render.st.light_draws,
               g_render.st.rim_tris, g_render.st.shadow_tris, g_render.st.fx_sprites, g_render.st.fx_px,
               g_render.st.shadow_px);
    }
    sceDisplayWaitVblankStart();
    g_draw = sceGuSwapBuffers();
    return tris;
}

static void norm2(float v[3]) { float l = sqrtf(v[0] * v[0] + v[2] * v[2]) + 1e-6f; v[0] /= l; v[2] /= l; v[1] = 0; }

/* knight swings at the demon: spark at the swing phase, dust at the loop start */
static void tick(float dt, const float fwd[3], int fx_events)
{
    float dur = anim_duration(&g_k.anim, g_k.clip);
    float t0 = fmodf(g_k.t, dur), t1 = fmodf(g_k.t + dt, dur);
    g_k.t += dt;
    g_d.t += dt;
    pose(&g_k);
    if (g_d.ok) pose(&g_d);
    if (!fx_events) return;
    float hit = dur * 0.42f;
    if (t0 < hit && t1 >= hit) {
        float p[3] = {g_k.pos[0] + fwd[0] * 1.5f, g_k.pos[1] + 1.25f, g_k.pos[2] + fwd[2] * 1.5f};
        float d[3] = {-fwd[0], 0.2f, -fwd[2]};
        fx_hit_spark(p, d, ((int)(g_k.t / dur)) % 3 == 2);
    }
    if (t1 < t0) fx_roll_dust(g_k.pos, g_k.yaw);
}

int main(void)
{
    u8 *f;
    char path[128];
    int n = ash_load_file("scene.ashm", &f, path, sizeof(path));
    if (n < 0 || scene_load(&g_sc0, f, n) < 0) { printf("rtest: no scene.ashm\n"); sceKernelExitGame(); return 0; }
    g_sc = g_sc0;
    {
        u8 *fv;
        int nv = ash_load_file("scene_vlit.ashm", &fv, NULL, 0);
        g_has_vlit = nv > 0 && scene_load(&g_sc_v, fv, nv) == 0;
        printf("rtest: vertex-lit variant %s\n", g_has_vlit ? "yes" : "no");
    }
    printf("rtest: %s %d B, %u tris, %u batches\n", path, n, (unsigned)(g_sc.hdr->n_idx / 3), (unsigned)g_sc.hdr->n_batch);
    render_init();
    n = ash_load_file("scene.ashmip", &f, NULL, 0);
    g_has_mips = n > 0 && render_mips_load(&g_mips, f, n) == 0;
    printf("rtest: mips %s (%d B)\n", g_has_mips ? "yes" : "no", n);
    n = ash_load_file("scene.ashl", &f, NULL, 0);
    g_has_zone = n > 0 && render_zone_load(&g_zone, f, n) == 0;
    if (g_has_zone) {
        render_zone_apply(&g_zone);
        fx_zone_fires(&g_zone);
        printf("rtest: zone %d B: %u lights, %u subsets, %u light tris, lightmap %ux%u, fog %.0f..%.0f %08x\n", n,
               (unsigned)g_zone.hdr->n_light, (unsigned)g_zone.hdr->n_sub, (unsigned)(g_zone.hdr->n_idx / 3),
               (unsigned)g_zone.hdr->lm_w, (unsigned)g_zone.hdr->lm_h, g_zone.hdr->fog_near, g_zone.hdr->fog_far,
               (unsigned)g_zone.hdr->fog_color);
    }
    n = ash_load_file("scene.pcol", &f, NULL, 0);
    g_has_col = n > 0 && pcol_load(&g_col, f, n) == 0;
    if (load_actor(&g_k, "knight.ashc", "knight.ashanim", "attack", "idle") < 0) printf("rtest: no knight\n");
    load_actor(&g_d, "demon.ashc", "demon.ashanim", "idle", NULL);
    float P[3] = {-3, 199, -12}, D[3] = {-3, 198, -29.5f}, pyaw = 0, dyaw = 0;
    {
        u8 *buf;
        if (ash_load_file("scene.spawn", &buf, NULL, 0) > 0) {
            char model[16];
            sscanf((const char *)buf, "player %f %f %f %f", &P[0], &P[1], &P[2], &pyaw);
            const char *l2 = strstr((const char *)buf, "target");
            if (l2) sscanf(l2, "target %15s %f %f %f %f", model, &D[0], &D[1], &D[2], &dyaw);
        }
    }
    if (g_has_col) pcol_floor(&g_col, D[0], D[2], D[1] + 2, D[1] - 5, 0.5f, &D[1]);
    float fwd[3] = {D[0] - P[0], 0, D[2] - P[2]};
    norm2(fwd);
    /* knight 4.5 m in front of the demon, facing it */
    float K[3] = {D[0] - fwd[0] * 4.5f, P[1], D[2] - fwd[2] * 4.5f};
    if (g_has_col) pcol_floor(&g_col, K[0], K[2], K[1] + 3, K[1] - 6, 0.5f, &K[1]);
    memcpy(g_k.pos, K, sizeof(K));
    g_k.yaw = atan2f(fwd[0], fwd[2]);
    memcpy(g_d.pos, D, sizeof(D));
    g_d.yaw = atan2f(-fwd[0], -fwd[2]);
    float side[3] = {fwd[2], 0, -fwd[0]};
    float mid[3] = {(K[0] + D[0]) * 0.5f, K[1], (K[2] + D[2]) * 0.5f};
    printf("rtest: knight (%.2f %.2f %.2f) demon (%.2f %.2f %.2f)\n", K[0], K[1], K[2], D[0], D[1], D[2]);

    /* torch camera: zone light nearest to the knight on its floor */
    float T[3] = {K[0] + side[0] * 6, K[1] + 2, K[2] + side[2] * 6};
    if (g_has_zone) {
        float bd = 1e9f;
        for (u32 i = 0; i < g_zone.hdr->n_light; i++) {
            const AshlLight *l = &g_zone.light[i];
            float dy = l->pos[1] - K[1];
            if (dy < -1 || dy > 6) continue;
            float d = (l->pos[0] - K[0]) * (l->pos[0] - K[0]) + (l->pos[2] - K[2]) * (l->pos[2] - K[2]);
            if (d < bd) { bd = d; memcpy(T, l->pos, sizeof(T)); }
        }
    }
    float tk[3] = {K[0] - T[0], 0, K[2] - T[2]};
    norm2(tk);
    Cam cams[4] = {
        {{K[0] - fwd[0] * 4.5f + side[0] * 1.2f, K[1] + 2.3f, K[2] - fwd[2] * 4.5f + side[2] * 1.2f},
         {mid[0], mid[1] + 1.8f, mid[2]}, 60, "hero"},
        {{K[0] - fwd[0] * 9 + side[0] * 3, K[1] + 6.0f, K[2] - fwd[2] * 9 + side[2] * 3},
         {mid[0], mid[1] + 1.0f, mid[2]}, 60, "wide"},
        {{K[0] - fwd[0] * 3 - side[0] * 1.5f, K[1] + 0.9f, K[2] - fwd[2] * 3 - side[2] * 1.5f},
         {K[0] + fwd[0] * 40, K[1] + 0.2f, K[2] + fwd[2] * 40}, 60, "floor"},
        {{T[0] + tk[0] * 7.5f + side[0] * 2, T[1] + 0.3f, T[2] + tk[2] * 7.5f + side[2] * 2},
         {T[0], T[1] - 1.5f, T[2]}, 60, "torch"},
    };

    int orbit = 90, warm = 45;
    {
        u8 *buf;
        if (ash_load_file("rtest.txt", &buf, NULL, 0) > 0) sscanf((const char *)buf, "%d %d", &orbit, &warm);
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
    sceGuFinish();
    sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);
    sceDisplayWaitVblankStart();
    sceGuDisplay(GU_TRUE);
    g_draw = fbp0;

    const float dt = 1.0f / 30.0f;
    char name[64];
    /* stills: same pose for before / after / cover */
    for (int c = 0; c < 4; c++) {
        fx_reset();
        if (g_has_zone) fx_zone_fires(&g_zone);
        g_k.t = 0.0f;
        for (int i = 0; i < warm; i++) {   /* fx warm-up (not dumped): fires burn, a swing hits, dust */
            tick(dt, fwd, 1);
            frame(&cams[c], RF_ALL, dt, NULL);
        }
        snprintf(name, sizeof(name), "c%d_before.raw", c);
        printf("rtest: cam %d %s\n", c, cams[c].name);
        frame(&cams[c], 0, 0, name);
        snprintf(name, sizeof(name), "c%d_after.raw", c);
        frame(&cams[c], RF_ALL, 0, name);
        snprintf(name, sizeof(name), "c%d_cover.raw", c);
        frame(&cams[c], RF_ALL | RF_DEBUG_COVER, 0, name);
        /* one effect at a time: cost of each (PSPEST line per dump) */
        snprintf(name, sizeof(name), "c%d_lmdbg.raw", c);
        frame(&cams[c], RF_LIGHTPASS | RF_DEBUG_LM, 0, name);
        for (int b = 0; b < 8; b++) {
            snprintf(name, sizeof(name), "c%d_only%02x.raw", c, 1 << b);
            frame(&cams[c], 1u << b, 0, name);
        }
    }
    /* orbit video: the same path twice (before, after) */
    for (int pass = 0; pass < 2; pass++) {
        fx_reset();
        if (g_has_zone) fx_zone_fires(&g_zone);
        g_k.t = 0; g_d.t = 0;
        u32 fl = pass ? RF_ALL : 0;
        for (int i = 0; i < 20; i++) { tick(dt, fwd, pass); g_render.flags = fl; fx_update(pass ? dt : 0); }
        for (int i = 0; i < orbit; i++) {
            float a = -0.9f + 2.2f * i / (float)orbit;
            float r = 9.5f;
            Cam c = {{mid[0] + (side[0] * cosf(a) - fwd[0] * sinf(a)) * r, K[1] + 3.2f,
                      mid[2] + (side[2] * cosf(a) - fwd[2] * sinf(a)) * r},
                     {mid[0], mid[1] + 1.6f, mid[2]}, 60, "orbit"};
            tick(dt, fwd, pass);
            snprintf(name, sizeof(name), "v_%s_%04d.raw", pass ? "after" : "before", i);
            frame(&c, fl, dt, name);
        }
        /* then a slow push towards the torch camera (light pass, fire, flicker) */
        int dolly = orbit * 2 / 3;
        for (int i = 0; i < dolly; i++) {
            float u = i / (float)dolly, back = 4.0f * (1.0f - u);
            Cam c = cams[3];
            c.eye[0] += tk[0] * back; c.eye[1] += 1.0f * (1.0f - u); c.eye[2] += tk[2] * back;
            tick(dt, fwd, pass);
            snprintf(name, sizeof(name), "v_%s_%04d.raw", pass ? "after" : "before", orbit + i);
            frame(&c, fl, dt, name);
        }
    }
    printf("rtest: done\n");
    sceGuTerm();
    sceKernelExitGame();
    return 0;
}
