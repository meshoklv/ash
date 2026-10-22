/* Ash render module: fog, mips, second light pass, character light + rim, blob shadows. See render.h. */
#include "render.h"

#include <math.h>
#include <pspgum.h>
#include <pspkernel.h>
#include <string.h>

RenderState g_render;

static ScePspFMatrix4 g_view, g_proj;
static float g_eye[3], g_right[3], g_up[3], g_time;
static float g_flicker = 1.0f;

/* procedural textures (RGBA8888, unswizzled, in RAM): blob shadow, rim ramp */
static u32 __attribute__((aligned(16))) g_tex_blob[32 * 32];
static u32 __attribute__((aligned(16))) g_tex_rim[32 * 32];
static int g_tex_ready;

#define SAT(x) ((x) < 0 ? 0 : (x) > 255 ? 255 : (x))
static u32 rgba(int r, int g, int b, int a) { return ((u32)SAT(a) << 24) | ((u32)SAT(b) << 16) | ((u32)SAT(g) << 8) | (u32)SAT(r); }

/* debug coverage: every fragment of a pass becomes white (logic op SET, no blend); post.py counts them */
int render_cover_begin(void)
{
    if (!(g_render.flags & RF_DEBUG_COVER)) return 0;
    sceGuDisable(GU_BLEND);
    sceGuEnable(GU_COLOR_LOGIC_OP);
    sceGuLogicalOp(GU_SET);
    sceGuDisable(GU_FOG);
    return 1;
}

void render_cover_end(void)
{
    if (!(g_render.flags & RF_DEBUG_COVER)) return;
    sceGuDisable(GU_COLOR_LOGIC_OP);
}

static float smooth(float e0, float e1, float x)
{
    float t = (x - e0) / (e1 - e0);
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    return t * t * (3 - 2 * t);
}

static void make_textures(void)
{
    for (int y = 0; y < 32; y++)
        for (int x = 0; x < 32; x++) {
            float dx = (x + 0.5f) / 16.0f - 1.0f, dy = (y + 0.5f) / 16.0f - 1.0f;
            float r = sqrtf(dx * dx + dy * dy);
            /* blob: dense core, soft edge */
            float a = 1.0f - smooth(0.25f, 1.0f, r);
            g_tex_blob[y * 32 + x] = rgba(255, 255, 255, (int)(a * a * 255));
            /* rim: u = (1 + N.right) / 2, v = (1 + N.up) / 2 (sphere map) -> r = |N_view.xy|; bright near the
               silhouette (r -> 1), stronger on the top edge (dy > 0 = normal up) [придумал] */
            float rim = smooth(0.55f, 0.97f, r) * (1.0f - smooth(0.985f, 1.2f, r));
            float top = 0.55f + 0.45f * (dy > 0 ? dy : 0);
            g_tex_rim[y * 32 + x] = rgba(255, 255, 255, (int)(rim * top * 255));
        }
    sceKernelDcacheWritebackRange(g_tex_blob, sizeof(g_tex_blob));
    sceKernelDcacheWritebackRange(g_tex_rim, sizeof(g_tex_rim));
    g_tex_ready = 1;
}

void render_init(void)
{
    memset(&g_render, 0, sizeof(g_render));
    /* the look src/game/main.c had on 14.10 ("before") */
    g_render.fog.near = 30.0f;
    g_render.fog.far = 110.0f;
    g_render.fog.color = 0xff1a1410;
    g_render.fog.clear = 0xff1a1410;
    g_render.chl.dir[0][0] = -0.35f; g_render.chl.dir[0][1] = 0.8f; g_render.chl.dir[0][2] = -0.45f;
    g_render.chl.col[0] = 0xff86b2d9;
    g_render.chl.ambient = 0xff4a4a4a;
    g_render.chl.point_gain = 0.0f;
    g_render.mip_bias = 0.0f;
    g_render.mip_filter = GU_LINEAR_MIPMAP_LINEAR;
    g_render.shadow_alpha = 0.7f;
    if (!g_tex_ready) make_textures();
    fx_reset();
}

/* ------------------------------------------------------------------ zone */

int render_zone_load(RenderZone *z, u8 *file, int size)
{
    memset(z, 0, sizeof(*z));
    if (size < (int)sizeof(AshlHdr)) return -1;
    const AshlHdr *h = (const AshlHdr *)file;
    if (memcmp(h->magic, "ASHL", 4) != 0 || h->version != 1) return -2;
    z->file = file;
    z->hdr = h;
    z->light = (const AshlLight *)(file + h->off_light);
    z->sub = (const AshlSub *)(file + h->off_sub);
    z->idx = (const u16 *)(file + h->off_idx);
    z->clut = (const u32 *)(file + h->off_clut);
    z->pix = file + h->off_pix;
    memcpy(z->clut_anim, z->clut, sizeof(z->clut_anim));
    sceKernelDcacheWritebackAll();
    return 0;
}

void render_zone_apply(const RenderZone *z)
{
    g_render.zone = z;
    if (!z) return;
    const AshlHdr *h = z->hdr;
    g_render.fog.near = h->fog_near;
    g_render.fog.far = h->fog_far;
    g_render.fog.color = h->fog_color;
    g_render.fog.clear = h->clear_color;
    for (int k = 0; k < 3; k++) {
        for (int i = 0; i < 3; i++) g_render.chl.dir[k][i] = h->chl_dir[k][i];
        g_render.chl.col[k] = h->chl_col[k];
    }
    g_render.chl.ambient = h->chl_ambient;
    g_render.chl.point_gain = 1.0f;
}

/* ------------------------------------------------------------------ frame */

void render_begin_frame(const ScePspFMatrix4 *view, const ScePspFMatrix4 *proj, const float eye[3], float dt)
{
    g_view = *view;
    g_proj = *proj;
    for (int i = 0; i < 3; i++) g_eye[i] = eye[i];
    g_right[0] = view->x.x; g_right[1] = view->y.x; g_right[2] = view->z.x;
    g_up[0] = view->x.y; g_up[1] = view->y.y; g_up[2] = view->z.y;
    g_time += dt;
    /* torch flicker: two incommensurate sines + a fast wobble, +-15 % [придумал] */
    g_flicker = 1.0f + 0.09f * sinf(g_time * 7.3f) + 0.05f * sinf(g_time * 17.9f + 1.3f) + 0.03f * sinf(g_time * 31.0f);
    memset(&g_render.st, 0, sizeof(g_render.st));
    fx_update(dt);
    sceGuClearColor((g_render.flags & RF_FOG) ? g_render.fog.clear : 0xff1a1410);
    render_fog_apply();
    /* lightmap palette flicker: c' = c * f (1 KB per frame) */
    const RenderZone *z = g_render.zone;
    if (z && (g_render.flags & RF_LIGHTPASS)) {
        u32 *d = ((RenderZone *)z)->clut_anim;
        for (int i = 0; i < 256; i++) {
            u32 c = z->clut[i];
            int r = (int)((c & 255) * g_flicker);
            int g = (int)(((c >> 8) & 255) * g_flicker);
            int b = (int)(((c >> 16) & 255) * g_flicker);
            d[i] = rgba(r, g, b, 255);
        }
        sceKernelDcacheWritebackRange(d, 1024);
    }
}

void render_end_frame(void)
{
    sceGuDisable(GU_BLEND);
    sceGuDepthMask(GU_FALSE);
    sceGuEnable(GU_FOG);
}

/* ------------------------------------------------------------------ fog */

void render_fog_apply(void)
{
    if (g_render.flags & RF_FOG) sceGuFog(g_render.fog.near, g_render.fog.far, g_render.fog.color);
    else sceGuFog(30.0f, 110.0f, 0xff1a1410);   /* the old look */
    sceGuEnable(GU_FOG);
}

int render_fog_visible(float cx, float cy, float cz, float radius)
{
    if (!(g_render.flags & RF_FOG)) return 1;
    float dx = cx - g_eye[0], dy = cy - g_eye[1], dz = cz - g_eye[2];
    float far = g_render.fog.far + radius;
    return dx * dx + dy * dy + dz * dz < far * far;
}

/* ------------------------------------------------------------------ mips */

int render_mips_load(RenderMips *m, u8 *file, int size)
{
    memset(m, 0, sizeof(*m));
    if (size < 16 || memcmp(file, "AMIP", 4) != 0 || ((u32 *)file)[1] != 1) return -1;
    m->file = file;
    m->n_tex = ((u32 *)file)[2];
    m->e = (const AmipEntry *)(file + 16);
    sceKernelDcacheWritebackAll();
    return 0;
}

void render_tex_bind(int psm, int w, int h, const void *clut, const void *pix, const RenderMips *mp, int tex)
{
    sceGuClutMode(GU_PSM_8888, 0, psm == GU_PSM_T4 ? 0x0f : 0xff, 0);
    sceGuClutLoad(psm == GU_PSM_T4 ? 2 : 32, clut);
    const AmipEntry *e = NULL;
    if ((g_render.flags & RF_MIPS) && mp && tex >= 0 && (u32)tex < mp->n_tex && mp->e[tex].n_levels > 0 &&
        mp->e[tex].w[0] == w && mp->e[tex].h[0] == h)
        e = &mp->e[tex];
    int n = e ? e->n_levels : 0;
    sceGuTexMode(psm, n, 0, 1);
    sceGuTexImage(0, w, h, w, pix);
    for (int l = 0; l < n; l++)
        sceGuTexImage(l + 1, e->w[l + 1], e->h[l + 1], e->w[l + 1], mp->file + e->off[l]);
    if (n) {
        sceGuTexLevelMode(GU_TEXTURE_AUTO, g_render.mip_bias);
        sceGuTexFilter(g_render.mip_filter, GU_LINEAR);
    } else {
        sceGuTexFilter(GU_LINEAR, GU_LINEAR);
    }
    sceGuTexFlush();
}

/* ------------------------------------------------------------------ second light pass */

static void lm_texmat(const float off[3], const float scale[3])
{
    const AshlHdr *h = g_render.zone->hdr;
    float ix = 1.0f / h->lm_sx, iz = 1.0f / h->lm_sz;
    ScePspFMatrix4 m;
    memset(&m, 0, sizeof(m));
    float ox = off ? off[0] : 0, oz = off ? off[2] : 0;
    float sx = scale ? scale[0] : 1, sz = scale ? scale[2] : 1;
    /* s = (x_world - x0) / sx_lm, t = (z_world - z0) / sz_lm, q = 1; x_world = ox + x_model * sx */
    m.x.x = sx * ix;
    m.z.y = sz * iz;
    m.w.x = (ox - h->lm_x0) * ix;
    m.w.y = (oz - h->lm_z0) * iz;
    m.w.z = 1.0f;
    /* through gum: sceGumDrawArray re-uploads gum's (identity) texture matrix if it is dirty */
    sceGumMatrixMode(GU_TEXTURE);
    sceGumLoadMatrix(&m);
    sceGumMatrixMode(GU_MODEL);
}

void render_light_pass_begin(void)
{
    const RenderZone *z = g_render.zone;
    const AshlHdr *h = z->hdr;
    sceGuEnable(GU_BLEND);
    sceGuBlendFunc(GU_ADD, GU_FIX, GU_FIX, 0xffffffff, 0xffffffff);  /* additive: dst + light */
    sceGuFog(g_render.fog.near, g_render.fog.far, 0xff000000);      /* fades to "no change" */
    render_cover_begin();
    if (g_render.flags & RF_DEBUG_LM) { sceGuDisable(GU_BLEND); sceGuDisable(GU_FOG); }   /* debug: lightmap as is */
    sceGuDepthMask(GU_TRUE);
    sceGuDisable(GU_ALPHA_TEST);
    sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGB);
    sceGuClutMode(GU_PSM_8888, 0, 0xff, 0);
    sceGuClutLoad(32, z->clut_anim);
    sceGuTexMode(GU_PSM_T8, 0, 0, 1);
    sceGuTexImage(0, h->lm_w, h->lm_h, h->lm_w, z->pix);
    sceGuTexFilter(GU_LINEAR, GU_LINEAR);
    sceGuTexWrap(GU_CLAMP, GU_CLAMP);
    sceGuTexFlush();
    sceGuTexMapMode(GU_TEXTURE_MATRIX, 0, 0);
    sceGuTexProjMapMode(GU_POSITION);
    sceGuColor(0xffff00ff);
}

void render_light_pass_draw(int sub, int vtype, const void *verts, const float pos_off[3], const float pos_scale[3])
{
    const AshlSub *s = &g_render.zone->sub[sub];
    lm_texmat(pos_off, pos_scale);
    sceGumDrawArray(GU_TRIANGLES, vtype | GU_INDEX_16BIT | GU_TRANSFORM_3D, s->idx_count,
                    g_render.zone->idx + s->idx_start, verts);
    g_render.st.light_tris += s->idx_count / 3;
    g_render.st.light_draws++;
}

void render_light_pass_end(void)
{
    render_cover_end();
    sceGuTexMapMode(GU_TEXTURE_COORDS, 0, 0);
    sceGuTexWrap(GU_REPEAT, GU_REPEAT);
    sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
    sceGuDisable(GU_BLEND);
    sceGuDepthMask(GU_FALSE);
    sceGuEnable(GU_TEXTURE_2D);
    sceGuColor(0xffffffff);
    render_fog_apply();
}

void render_light_pass_ashm(const void *vert_base, const void *batch_table, int batch_stride)
{
    const RenderZone *z = g_render.zone;
    if (!z || !(g_render.flags & RF_LIGHTPASS) || z->hdr->n_sub == 0) return;
    render_light_pass_begin();
    sceGumMatrixMode(GU_MODEL);
    sceGumLoadIdentity();
    for (u32 i = 0; i < z->hdr->n_sub; i++) {
        const u8 *b = (const u8 *)batch_table + z->sub[i].batch * batch_stride;
        u32 vert_start = *(const u32 *)(b + 4);
        /* .ashm vertex: f32 uv, u32 colour, f32 xyz (24 B) */
        render_light_pass_draw((int)i, GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF,
                               (const u8 *)vert_base + vert_start * 24, NULL, NULL);
    }
    render_light_pass_end();
}

/* ------------------------------------------------------------------ characters */

static int nearest_light(const float p[3], float *d2_out)
{
    const RenderZone *z = g_render.zone;
    int best = -1;
    float bd = 1e30f;
    if (!z) return -1;
    for (u32 i = 0; i < z->hdr->n_light; i++) {
        const AshlLight *l = &z->light[i];
        float dx = l->pos[0] - p[0], dy = l->pos[1] - p[1], dz = l->pos[2] - p[2];
        float d2 = dx * dx + dy * dy * 4.0f + dz * dz;   /* other floors count as far */
        if (d2 < bd) { bd = d2; best = (int)i; }
    }
    if (d2_out) *d2_out = bd;
    return best;
}

static u32 scale_col(u32 c, float k)
{
    return rgba((int)((c & 255) * k), (int)(((c >> 8) & 255) * k), (int)(((c >> 16) & 255) * k), 255);
}

void render_char_light_begin(const float pos[3])
{
    sceGuEnable(GU_LIGHTING);
    sceGuLightMode(GU_SINGLE_COLOR);
    sceGuColor(0xffffffff);
    sceGuAmbientColor(0xffffffff);
    if (!(g_render.flags & RF_CHARLIGHT)) {
        ScePspFVector3 d = {-0.35f, 0.8f, -0.45f};
        sceGuLight(0, GU_DIRECTIONAL, GU_DIFFUSE, &d);
        sceGuLightColor(0, GU_DIFFUSE, 0xff86b2d9);
        sceGuAmbient(0xff4a4a4a);
        sceGuEnable(GU_LIGHT0);
        sceGuDisable(GU_LIGHT1); sceGuDisable(GU_LIGHT2); sceGuDisable(GU_LIGHT3);
        return;
    }
    for (int k = 0; k < 3; k++) {
        ScePspFVector3 d = {g_render.chl.dir[k][0], g_render.chl.dir[k][1], g_render.chl.dir[k][2]};
        sceGuLight(k, GU_DIRECTIONAL, GU_DIFFUSE, &d);
        sceGuLightColor(k, GU_DIFFUSE, g_render.chl.col[k]);
        if (g_render.chl.col[k] & 0xffffff) sceGuEnable(GU_LIGHT0 + k); else sceGuDisable(GU_LIGHT0 + k);
    }
    sceGuAmbient(g_render.chl.ambient);
    float d2;
    int li = g_render.chl.point_gain > 0 ? nearest_light(pos, &d2) : -1;
    if (li >= 0 && d2 < 18.0f * 18.0f) {
        const AshlLight *l = &g_render.zone->light[li];
        ScePspFVector3 lp = {l->pos[0], l->pos[1], l->pos[2]};
        sceGuLight(3, GU_POINTLIGHT, GU_DIFFUSE, &lp);
        float k = g_render.chl.point_gain * g_flicker * (l->strength > 2.5f ? 1.0f : 0.8f);
        sceGuLightColor(3, GU_DIFFUSE, scale_col(l->color, k > 1.0f ? 1.0f : k));
        /* 1 / (a0 + a2 d^2): full at ~1.5 m, ~25 % at 2x the DS1 radius [придумал] */
        float r = l->radius > 1.0f ? l->radius : 1.0f;
        sceGuLightAtt(3, 0.6f, 0.0f, 2.4f / (r * r));
        sceGuEnable(GU_LIGHT3);
    } else {
        sceGuDisable(GU_LIGHT3);
    }
}

void render_char_light_end(void)
{
    sceGuDisable(GU_LIGHTING);
}

void render_rim_begin(void)
{
    /* sphere map from the view-space normal: u = (1 + N.right)/2, v = (1 + N.up)/2 via lights 2/3 as vectors */
    sceGuDisable(GU_LIGHTING);
    ScePspFVector3 r = {g_right[0], g_right[1], g_right[2]}, u = {g_up[0], g_up[1], g_up[2]};
    sceGuLight(2, GU_DIRECTIONAL, GU_DIFFUSE, &r);
    sceGuLight(3, GU_DIRECTIONAL, GU_DIFFUSE, &u);
    sceGuTexMapMode(GU_ENVIRONMENT_MAP, 2, 3);
    sceGuTexMode(GU_PSM_8888, 0, 0, 0);
    sceGuTexImage(0, 32, 32, 32, g_tex_rim);
    sceGuTexFilter(GU_LINEAR, GU_LINEAR);
    sceGuTexWrap(GU_CLAMP, GU_CLAMP);
    sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
    sceGuTexFlush();
    sceGuDisable(GU_ALPHA_TEST);
    sceGuDepthMask(GU_TRUE);
    sceGuEnable(GU_BLEND);
    sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_FIX, 0, 0xffffffff);
    sceGuFog(g_render.fog.near, g_render.fog.far, 0xff000000);  /* additive: fade to nothing */
    render_cover_begin();
    /* warm-white rim, ~35 % [придумал, на глаз] */
    sceGuColor(0x58c8e0ff);
}

void render_rim_end(void)
{
    render_cover_end();
    sceGuTexMapMode(GU_TEXTURE_COORDS, 0, 0);
    sceGuTexWrap(GU_REPEAT, GU_REPEAT);
    sceGuDisable(GU_BLEND);
    sceGuDepthMask(GU_FALSE);
    sceGuColor(0xffffffff);
    render_fog_apply();
}

/* ------------------------------------------------------------------ blob shadows */

typedef struct { float u, v; u32 color; float x, y, z; } TVert;

void render_shadow_blob(const float pos[3], float radius, RenderFloorFn floor, void *ctx)
{
    if (!(g_render.flags & RF_SHADOWS)) return;
    TVert *v = sceGuGetMemory(9 * sizeof(TVert));
    u16 *ix = sceGuGetMemory(24 * sizeof(u16));
    float base = pos[1];
    float hc;
    if (floor && floor(ctx, pos[0], pos[2], pos[1] + 0.8f, pos[1] - 4.0f, &hc)) base = hc;
    float lift = pos[1] - base;                  /* in the air: fainter + wider */
    float k = 1.0f - lift / 2.5f;
    if (k <= 0.05f) return;
    float rad = radius * (1.0f + lift * 0.25f);
    int a = (int)(255 * g_render.shadow_alpha * k);
    for (int j = 0; j < 3; j++)
        for (int i = 0; i < 3; i++) {
            TVert *t = &v[j * 3 + i];
            t->x = pos[0] + (i - 1) * rad;
            t->z = pos[2] + (j - 1) * rad;
            float h = base;
            if (floor && !(i == 1 && j == 1)) {
                if (!floor(ctx, t->x, t->z, base + 0.6f, base - 1.5f, &h)) h = base;
            }
            t->y = h + 0.04f;
            t->u = i * 0.5f;
            t->v = j * 0.5f;
            t->color = (u32)a << 24;
        }
    int n = 0;
    for (int j = 0; j < 2; j++)
        for (int i = 0; i < 2; i++) {
            int a0 = j * 3 + i, a1 = a0 + 1, b0 = a0 + 3, b1 = b0 + 1;
            ix[n++] = a0; ix[n++] = b0; ix[n++] = a1;
            ix[n++] = a1; ix[n++] = b0; ix[n++] = b1;
        }
    sceGumMatrixMode(GU_MODEL);
    sceGumLoadIdentity();
    sceGuEnable(GU_BLEND);
    sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
    render_cover_begin();
    sceGuDepthMask(GU_TRUE);
    sceGuDisable(GU_CULL_FACE);
    sceGuDisable(GU_ALPHA_TEST);
    sceGuTexMode(GU_PSM_8888, 0, 0, 0);
    sceGuTexImage(0, 32, 32, 32, g_tex_blob);
    sceGuTexFilter(GU_LINEAR, GU_LINEAR);
    sceGuTexWrap(GU_CLAMP, GU_CLAMP);
    sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
    sceGuTexFlush();
    sceGumDrawArray(GU_TRIANGLES, GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_INDEX_16BIT |
                    GU_TRANSFORM_3D, 24, ix, v);
    render_cover_end();
    sceGuTexWrap(GU_REPEAT, GU_REPEAT);
    sceGuEnable(GU_CULL_FACE);
    sceGuDisable(GU_BLEND);
    sceGuDepthMask(GU_FALSE);
    render_fog_apply();
    g_render.st.shadow_tris += 8;
    /* fill estimate: disc area on screen */
    float dx = pos[0] - g_eye[0], dy = pos[1] - g_eye[1], dz = pos[2] - g_eye[2];
    float d = sqrtf(dx * dx + dy * dy + dz * dz) + 0.1f;
    float px = 2.0f * rad * g_proj.y.y * 136.0f / d;
    g_render.st.shadow_px += (int)(px * px * 0.6f);
}

/* used by fx.c */
void render_internal_basis(float right[3], float up[3], float eye[3], float *proj_yy, float *flicker)
{
    for (int i = 0; i < 3; i++) { right[i] = g_right[i]; up[i] = g_up[i]; eye[i] = g_eye[i]; }
    *proj_yy = g_proj.y.y;
    *flicker = g_flicker;
}
