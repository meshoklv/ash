/* Fast zone drawing: strips, cached display lists, texture sort, mips, hot VRAM (render_world.h, the perf-night design notes). */
#include "render_world.h"

#include <malloc.h>
#include <math.h>
#include <pspgum.h>
#include <pspkernel.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VTYPE (GU_TEXTURE_16BIT | GU_COLOR_5650 | GU_VERTEX_16BIT | GU_INDEX_16BIT | GU_TRANSFORM_3D)
#define NONE 0xffffffffu

/* ------------------------------------------------------------------ strips */

typedef struct { u32 key; int tri; } EdgeH;

static inline u32 ehash(u32 k) { return k * 2654435761u; }

static int edge_find(const EdgeH *h, u32 mask, u32 key, const u8 *used)
{
    for (u32 i = ehash(key) & mask; h[i].tri >= 0; i = (i + 1) & mask)
        if (h[i].key == key && !used[h[i].tri]) return h[i].tri;
    return -1;
}

static inline int third(const u16 *t, u16 u, u16 v)
{
    if (t[0] == u && t[1] == v) return t[2];
    if (t[1] == u && t[2] == v) return t[0];
    return t[1];   /* t[2] == u && t[0] == v */
}

int render_strip_better(int n_idx, int n_strip, float vert_ns)
{
    /* GE: every index = one vertex transform; every assembled triangle (the degenerate joins too) = setup
       [psp_cost.cfg: ge_tri_ns 40, vertex 60 ns + 0.75 ns/B; оценка] */
    const float tri_ns = 40.0f;
    float list = n_idx * vert_ns + (n_idx / 3) * tri_ns, strip = n_strip * vert_ns + (n_strip - 2) * tri_ns;
    return n_strip > 0 && strip < list * 0.97f;
}

int render_stripify(const u16 *idx, int n_idx, u16 *out)
{
    int nt = n_idx / 3, L = 0;
    if (nt <= 0) return 0;
    u32 hs = 16;
    while (hs < (u32)nt * 6) hs <<= 1;
    EdgeH *h = malloc(sizeof(EdgeH) * hs);
    u8 *used = calloc(nt, 1);
    u16 *P = malloc(sizeof(u16) * (nt + 2));
    if (!h || !used || !P) { free(h); free(used); free(P); return -1; }
    for (u32 i = 0; i < hs; i++) h[i].tri = -1;
    u32 mask = hs - 1;
    for (int t = 0; t < nt; t++) {
        const u16 *v = idx + 3 * t;
        if (v[0] == v[1] || v[1] == v[2] || v[0] == v[2]) { used[t] = 1; continue; }   /* degenerate: draws nothing */
        for (int e = 0; e < 3; e++) {
            u32 key = ((u32)v[e] << 16) | v[(e + 1) % 3];
            u32 i = ehash(key) & mask;
            while (h[i].tri >= 0) i = (i + 1) & mask;
            h[i].key = key;
            h[i].tri = t;
        }
    }
    for (int t = 0; t < nt; t++) {
        if (used[t]) continue;
        const u16 *v = idx + 3 * t;
        int r = 0;
        used[t] = 1;
        for (int k = 0; k < 3; k++) {   /* rotation whose (c, b) edge continues the strip */
            u16 b = v[(k + 1) % 3], c = v[(k + 2) % 3];
            if (edge_find(h, mask, ((u32)c << 16) | b, used) >= 0) { r = k; break; }
        }
        int n = 0;
        P[n++] = v[r]; P[n++] = v[(r + 1) % 3]; P[n++] = v[(r + 2) % 3];
        for (;;) {
            int k = n - 2;
            u16 a = P[n - 2], b = P[n - 1];
            u16 eu = (k & 1) ? b : a, ev = (k & 1) ? a : b;   /* odd triangles are wound the other way */
            int t2 = edge_find(h, mask, ((u32)eu << 16) | ev, used);
            if (t2 < 0) break;
            used[t2] = 1;
            P[n++] = (u16)third(idx + 3 * t2, eu, ev);
        }
        if (L > 0) {   /* join with degenerates; the new strip must start at an even position */
            out[L] = out[L - 1]; L++;
            out[L++] = P[0];
            if (L & 1) out[L++] = P[0];
        }
        memcpy(out + L, P, n * sizeof(u16));
        L += n;
    }
    free(h); free(used); free(P);
    return L;
}

/* ------------------------------------------------------------------ display list blocks */

static inline u32 f24(float f) { union { float f; u32 i; } u; u.f = f; return u.i >> 8; }

static void batch_matrix(const WorldBatch *b, const float *R, const float *t, ScePspFMatrix4 *m)
{
    const float *s = b->pos_scale, *o = b->pos_off;
    if (R) {
        m->x.x = R[0] * s[0]; m->x.y = R[3] * s[0]; m->x.z = R[6] * s[0]; m->x.w = 0;
        m->y.x = R[1] * s[1]; m->y.y = R[4] * s[1]; m->y.z = R[7] * s[1]; m->y.w = 0;
        m->z.x = R[2] * s[2]; m->z.y = R[5] * s[2]; m->z.z = R[8] * s[2]; m->z.w = 0;
        m->w.x = R[0] * o[0] + R[1] * o[1] + R[2] * o[2] + t[0];
        m->w.y = R[3] * o[0] + R[4] * o[1] + R[5] * o[2] + t[1];
        m->w.z = R[6] * o[0] + R[7] * o[1] + R[8] * o[2] + t[2];
        m->w.w = 1;
    } else {
        memset(m, 0, sizeof(*m));
        m->x.x = s[0]; m->y.y = s[1]; m->z.z = s[2];
        m->w.x = o[0]; m->w.y = o[1]; m->w.z = o[2]; m->w.w = 1;
    }
}

static void batch_buffers(const RenderWorld *rw, int bi, int *prim, int *count, const void **ib)
{
    const World *w = rw->w;
    const WorldBatch *b = &w->batch[bi];
    if ((rw->flags & RW_STRIPS) && rw->s_start && rw->s_start[bi] != NONE) {
        *prim = GU_TRIANGLE_STRIP; *count = rw->s_count[bi]; *ib = rw->sidx + rw->s_start[bi];
    } else {
        *prim = GU_TRIANGLES; *count = b->idx_count; *ib = w->idx + b->idx_start;
    }
}

/* the same GE words sceGuSetMatrix(GU_MODEL) + sceGuTexScale/Offset + sceGuDrawArray send, then RET */
static u32 *emit_block(const RenderWorld *rw, int bi, u32 *d)
{
    const WorldBatch *b = &rw->w->batch[bi];
    ScePspFMatrix4 m;
    batch_matrix(b, NULL, NULL, &m);
    const float *f = (const float *)&m;
    *d++ = 0x3A000000;
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 3; j++) *d++ = 0x3B000000 | f24(f[j + i * 4]);
    *d++ = 0x48000000 | f24(b->uv_scale[0]);
    *d++ = 0x49000000 | f24(b->uv_scale[1]);
    *d++ = 0x4A000000 | f24(b->uv_off[0]);
    *d++ = 0x4B000000 | f24(b->uv_off[1]);
    int prim, count;
    const void *ib;
    batch_buffers(rw, bi, &prim, &count, &ib);
    u32 ia = (u32)ib, va = (u32)(rw->w->vert + b->vert_start);
    *d++ = 0x12000000 | VTYPE;
    *d++ = 0x10000000 | ((ia >> 8) & 0xF0000);
    *d++ = 0x02000000 | (ia & 0xFFFFFF);
    *d++ = 0x10000000 | ((va >> 8) & 0xF0000);
    *d++ = 0x01000000 | (va & 0xFFFFFF);
    *d++ = 0x04000000 | (prim << 16) | (count & 0xFFFF);
    *d++ = 0x0B000000;
    return d;
}
#define BLOCK_WORDS 24

/* ------------------------------------------------------------------ init */

int render_world_init(RenderWorld *rw, World *w, const RenderMips *mp, int flags)
{
    memset(rw, 0, sizeof(*rw));
    rw->w = w;
    rw->flags = flags;
    const int nb = (int)w->w->n_batch, nt = (int)w->w->n_tex;
    rw->vis = malloc(sizeof(u16) * (nb + 1));
    rw->key = malloc(sizeof(u32) * (nb + 1));
    if (!rw->vis || !rw->key) return -1;

    if (flags & RW_STRIPS) {
        u32 total = 0;
        for (int i = 0; i < nb; i++) total += w->batch[i].idx_count;
        /* strips are almost always shorter than the list; a batch whose strip is not keeps its list */
        u16 *tmp = malloc(sizeof(u16) * 3 * 65536);
        rw->sidx = malloc(sizeof(u16) * (total + 64));
        rw->s_start = malloc(sizeof(u32) * nb);
        rw->s_count = malloc(sizeof(u16) * nb);
        if (!tmp || !rw->sidx || !rw->s_start || !rw->s_count) { free(tmp); return -2; }
        u32 at = 0;
        for (int i = 0; i < nb; i++) {
            const WorldBatch *b = &w->batch[i];
            rw->s_start[i] = NONE;
            rw->list_idx += b->idx_count;
            rw->tris += b->idx_count / 3;
            int n = b->idx_count <= 3 * 21845 ? render_stripify(w->idx + b->idx_start, b->idx_count, tmp) : -1;
            if (n > 0 && n < 65536 && render_strip_better(b->idx_count, n, 60.0f + 0.75f * 14)) {
                memcpy(rw->sidx + at, tmp, n * sizeof(u16));
                rw->s_start[i] = at;
                rw->s_count[i] = (u16)n;
                at += (n + 1) & ~1;   /* keep 4-byte alignment of each strip */
                rw->strip_idx += n;
                rw->n_strip_batches++;
            } else {
                rw->strip_idx += b->idx_count;
            }
        }
        free(tmp);
        u16 *shrunk = realloc(rw->sidx, sizeof(u16) * (at + 2));
        if (shrunk) rw->sidx = shrunk;
        rw->strip_bytes = at * 2;
    }
    if (flags & RW_CACHE) {
        int n = 0;
        for (int i = 0; i < nb; i++) if (!(w->batch[i].flags & WB_MESH)) n++;
        rw->dl = memalign(16, sizeof(u32) * BLOCK_WORDS * (n + 1));
        rw->dl_off = malloc(sizeof(u32) * nb);
        if (!rw->dl || !rw->dl_off) return -3;
        u32 *d = rw->dl;
        for (int i = 0; i < nb; i++) {
            if (w->batch[i].flags & WB_MESH) { rw->dl_off[i] = NONE; continue; }
            rw->dl_off[i] = d - rw->dl;
            d = emit_block(rw, i, d);
        }
        rw->dl_bytes = (d - rw->dl) * 4;
    }
    rw->n_lv = calloc(nt + 1, 1);
    rw->lv = calloc(nt + 1, sizeof(*rw->lv));
    rw->lw = calloc(nt + 1, sizeof(*rw->lw));
    rw->lh = calloc(nt + 1, sizeof(*rw->lh));
    if (!rw->n_lv || !rw->lv || !rw->lw || !rw->lh) return -4;
    for (int i = 0; i < nt; i++) {
        const WorldTex *t = &w->tex[i];
        rw->lv[i][0] = w->tpix[i];
        rw->lw[i][0] = t->w;
        rw->lh[i][0] = t->h;
        if ((flags & RW_MIPS) && mp && (u32)i < mp->n_tex) {
            const AmipEntry *e = &mp->e[i];
            if (e->n_levels > 0 && e->w[0] == t->w && e->h[0] == t->h && e->psm == t->psm) {
                rw->n_lv[i] = (u8)(e->n_levels > 7 ? 7 : e->n_levels);
                for (int l = 0; l < rw->n_lv[i]; l++) {
                    rw->lv[i][l + 1] = mp->file + e->off[l];
                    rw->lw[i][l + 1] = e->w[l + 1];
                    rw->lh[i][l + 1] = e->h[l + 1];
                }
            }
        }
    }
    sceKernelDcacheWritebackAll();
    return 0;
}

void render_world_free(RenderWorld *rw)
{
    free(rw->sidx); free(rw->s_start); free(rw->s_count); free(rw->dl); free(rw->dl_off);
    free(rw->n_lv); free(rw->lv); free(rw->lw); free(rw->lh); free(rw->vis); free(rw->key);
    memset(rw, 0, sizeof(*rw));
}

/* ------------------------------------------------------------------ VRAM */

static int tex_bytes(int psm, int w, int h) { int b = psm == GU_PSM_T4 ? w * h / 2 : w * h; return (b + 15) & ~15; }

int render_world_vram(RenderWorld *rw, void *vram, int bytes)
{
    World *w = rw->w;
    const int nt = (int)w->w->n_tex, nb = (int)w->w->n_batch;
    float *score = calloc(nt, sizeof(float));
    int *need = calloc(nt, sizeof(int)), *order = malloc(sizeof(int) * nt);
    if (!score || !need || !order) { free(score); free(need); free(order); return 0; }
    for (int i = 0; i < nb; i++) {
        const WorldBatch *b = &w->batch[i];
        /* sky / backdrop: on screen in every outdoor frame, large and minified */
        score[b->tex] += (b->flags & (WB_SKY | WB_FAR)) ? 4000.0f : (float)(b->idx_count / 3);
    }
    for (int i = 0; i < nt; i++) {
        const WorldTex *t = &w->tex[i];
        need[i] = (t->psm == GU_PSM_T4 ? 64 : 1024);
        for (int l = 0; l <= rw->n_lv[i]; l++) need[i] += tex_bytes(t->psm, rw->lw[i][l], rw->lh[i][l]);
        score[i] /= (float)need[i];
        order[i] = i;
    }
    for (int i = 1; i < nt; i++)   /* insertion sort, best score per byte first */
        for (int j = i; j > 0 && score[order[j]] > score[order[j - 1]]; j--) { int x = order[j]; order[j] = order[j - 1]; order[j - 1] = x; }
    u8 *dst = (u8 *)vram;
    int used = 0, n_in = 0;
    for (int k = 0; k < nt; k++) {
        int i = order[k];
        if (used + need[i] > bytes) continue;
        const WorldTex *t = &w->tex[i];
        int cl = t->psm == GU_PSM_T4 ? 64 : 1024;
        memcpy(dst, w->tclut[i], cl);
        w->tclut[i] = dst;
        dst += (cl + 15) & ~15;
        for (int l = 0; l <= rw->n_lv[i]; l++) {
            int nbt = tex_bytes(t->psm, rw->lw[i][l], rw->lh[i][l]);
            memcpy(dst, rw->lv[i][l], nbt);
            rw->lv[i][l] = dst;
            dst += nbt;
        }
        w->tpix[i] = rw->lv[i][0];
        used += need[i];
        n_in++;
    }
    sceKernelDcacheWritebackAll();
    printf("render_world: %d of %d textures in VRAM (%d KB, levels+CLUT)\n", n_in, nt, used / 1024);
    free(score); free(need); free(order);
    return used;
}

/* ------------------------------------------------------------------ drawing */

static float g_planes[6][4];

static void mat_mul(ScePspFMatrix4 *out, const ScePspFMatrix4 *a, const ScePspFMatrix4 *b)
{
    const float *A = (const float *)a, *B = (const float *)b;
    float *O = (float *)out;
    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 4; r++) {
            float s = 0;
            for (int k = 0; k < 4; k++) s += A[k * 4 + r] * B[c * 4 + k];
            O[c * 4 + r] = s;
        }
}

static void extract_planes(const ScePspFMatrix4 *m)
{
    float r[4][4];
    const float *a = (const float *)m;
    for (int row = 0; row < 4; row++)
        for (int col = 0; col < 4; col++) r[row][col] = a[col * 4 + row];
    for (int i = 0; i < 3; i++)
        for (int sgn = 0; sgn < 2; sgn++) {
            float *p = g_planes[i * 2 + sgn];
            float sign = sgn ? -1.0f : 1.0f;
            for (int k = 0; k < 4; k++) p[k] = r[3][k] + sign * r[i][k];
            float n = sqrtf(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
            if (n > 0) for (int k = 0; k < 4; k++) p[k] /= n;
        }
}

static int sphere_visible(const float c[3], float rad, int planes)
{
    for (int i = 0; i < planes; i++) {
        const float *p = g_planes[i];
        if (p[0] * c[0] + p[1] * c[1] + p[2] * c[2] + p[3] < -rad) return 0;
    }
    return 1;
}

typedef struct { int tex, alpha, cull; } DrawState;

static void bind_tex(RenderWorld *rw, DrawState *ds, int ti, WorldStats *st)
{
    if (ti == ds->tex) return;
    const World *w = rw->w;
    const WorldTex *t = &w->tex[ti];
    int t4 = t->psm == GU_PSM_T4;
    sceGuClutMode(GU_PSM_8888, 0, t4 ? 0x0f : 0xff, 0);
    sceGuClutLoad(t4 ? 16 / 8 : 256 / 8, w->tclut[ti]);
    int n = (rw->flags & RW_MIPS) ? rw->n_lv[ti] : 0;
    sceGuTexMode(t->psm, n, 0, 1);
    sceGuTexImage(0, t->w, t->h, t->w, w->tpix[ti]);
    for (int l = 1; l <= n; l++) sceGuTexImage(l, rw->lw[ti][l], rw->lh[ti][l], rw->lw[ti][l], rw->lv[ti][l]);
    if (n) {
        sceGuTexLevelMode(GU_TEXTURE_AUTO, g_render.mip_bias);
        sceGuTexFilter(g_render.mip_filter ? g_render.mip_filter : GU_LINEAR_MIPMAP_LINEAR, GU_LINEAR);
    } else {
        sceGuTexFilter(GU_LINEAR, GU_LINEAR);
    }
    sceGuTexFlush();
    ds->tex = ti;
    if (st) st->tex_changes++;
}

static void set_flags(DrawState *ds, int flags)
{
    int alpha = (flags & WB_ALPHA) ? 1 : 0;
    if (alpha != ds->alpha) {
        if (alpha) sceGuEnable(GU_ALPHA_TEST); else sceGuDisable(GU_ALPHA_TEST);
        ds->alpha = alpha;
    }
    int cull = (flags & WB_NOCULL) ? 0 : 1;
    if (cull != ds->cull) {
        if (cull) sceGuEnable(GU_CULL_FACE); else sceGuDisable(GU_CULL_FACE);
        ds->cull = cull;
    }
}

static void draw_batch(RenderWorld *rw, DrawState *ds, int bi, const float *R, const float *t, WorldStats *st)
{
    const World *w = rw->w;
    const WorldBatch *b = &w->batch[bi];
    bind_tex(rw, ds, b->tex, st);
    set_flags(ds, b->flags);
    if (!R && rw->dl && rw->dl_off[bi] != NONE) {
        sceGuCallList(rw->dl + rw->dl_off[bi]);
    } else {
        ScePspFMatrix4 m;
        batch_matrix(b, R, t, &m);
        sceGuSetMatrix(GU_MODEL, &m);
        sceGuTexScale(b->uv_scale[0], b->uv_scale[1]);
        sceGuTexOffset(b->uv_off[0], b->uv_off[1]);
        int prim, count;
        const void *ib;
        batch_buffers(rw, bi, &prim, &count, &ib);
        sceGuDrawArray(prim, VTYPE, count, ib, w->vert + b->vert_start);
    }
    if (st) { st->tris += b->idx_count / 3; st->batches++; }
}

static inline int mask_hit(const u32 *a, const u32 *b)
{
    return ((a[0] & b[0]) | (a[1] & b[1]) | (a[2] & b[2]) | (a[3] & b[3])) != 0;
}

static int cmp_u32(const void *a, const void *b)
{
    u32 x = *(const u32 *)a, y = *(const u32 *)b;
    return x < y ? -1 : x > y;
}

/* draws the collected batches: sorted by (state, texture) when RW_SORT, else in collection order */
static void flush(RenderWorld *rw, DrawState *ds, int n, WorldStats *st)
{
    const World *w = rw->w;
    if ((rw->flags & RW_SORT) && n > 1) {
        for (int k = 0; k < n; k++) {
            const WorldBatch *b = &w->batch[rw->vis[k]];
            u32 state = ((b->flags & WB_ALPHA) ? 2u : 0u) | ((b->flags & WB_NOCULL) ? 1u : 0u);
            rw->key[k] = (state << 28) | ((u32)(b->tex & 0xfff) << 16) | rw->vis[k];
        }
        qsort(rw->key, n, sizeof(u32), cmp_u32);
        for (int k = 0; k < n; k++) draw_batch(rw, ds, (int)(rw->key[k] & 0xffff), NULL, NULL, st);
    } else {
        for (int k = 0; k < n; k++) draw_batch(rw, ds, rw->vis[k], NULL, NULL, st);
    }
}

int render_world_draw(RenderWorld *rw, const ScePspFMatrix4 *proj, const ScePspFMatrix4 *view, const float eye[3],
                      WorldStats *st)
{
    World *w = rw->w;
    if (st) memset(st, 0, sizeof(*st));
    DrawState ds = {-1, -1, -1};
    ScePspFMatrix4 vp, sky = *proj;
    mat_mul(&vp, proj, view);
    extract_planes(&vp);
    sceGuSetMatrix(GU_VIEW, view);
    sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
    sceGuTexFilter(GU_LINEAR, GU_LINEAR);
    sceGuTexWrap(GU_REPEAT, GU_REPEAT);
    sceGuAlphaFunc(GU_GREATER, 0x80, 0xff);
    sceGuDisable(GU_LIGHTING);
    sceGuShadeModel(GU_SMOOTH);
    const int nb = (int)w->w->n_batch;

    /* 1. sky / backdrop: own far plane, no depth, no fog; order kept (clouds blend over the dome) */
    {
        const float n = 4.0f, f = 6500.0f;
        sky.z.z = (f + n) / (n - f);
        sky.w.z = 2.0f * f * n / (n - f);
        sceGuSetMatrix(GU_PROJECTION, &sky);
        sceGuDisable(GU_FOG);
        sceGuDisable(GU_DEPTH_TEST);
        sceGuDepthMask(GU_TRUE);
        for (int pass = 0; pass < 2; pass++) {
            if (pass == 1) {
                sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
                sceGuEnable(GU_BLEND);
            }
            for (int i = 0; i < nb; i++) {
                const WorldBatch *b = &w->batch[i];
                if (!(b->flags & WB_SKY) || ((b->flags & WB_BLEND) != 0) != (pass == 1)) continue;
                if (st) st->tested++;
                if (!sphere_visible(b->center, b->radius, 4)) continue;
                draw_batch(rw, &ds, i, NULL, NULL, st);
            }
        }
        sceGuDisable(GU_BLEND);
        sceGuEnable(GU_DEPTH_TEST);
        sceGuDepthMask(GU_FALSE);
        sceGuSetMatrix(GU_PROJECTION, proj);
    }
    /* 2. far terrain (no fog, no distance cull), then the world in fog: opaque/alpha-tested + Z, order-free */
    for (int pass = 0; pass < 2; pass++) {
        if (pass == 1) {
            sceGuFog(w->fog_near, w->fog_far, w->fog_color);
            sceGuEnable(GU_FOG);
        }
        float far2 = w->far_dist;
        int n = 0;
        for (int i = 0; i < nb; i++) {
            const WorldBatch *b = &w->batch[i];
            if (b->flags & (WB_SKY | WB_MESH)) continue;
            if (((b->flags & WB_FAR) != 0) != (pass == 0)) continue;
            if (st) st->tested++;
            if (pass == 1) {
                if (!mask_hit(b->mask, w->disp)) continue;
                if (b->flags & (WB_LOD_NEAR | WB_LOD_FAR)) {
                    float ch = w->w->chunk, hc = 0.5f * ch;
                    float cx = fabsf(((int)(b->chunk & 1023) - 512 + 0.5f) * ch - eye[0]) - hc;
                    float cy = fabsf(((int)((b->chunk >> 10) & 1023) - 512 + 0.5f) * ch - eye[1]) - hc;
                    float cz = fabsf(((int)((b->chunk >> 20) & 1023) - 512 + 0.5f) * ch - eye[2]) - hc;
                    cx = cx > 0.0f ? cx : 0.0f;
                    cy = cy > 0.0f ? cy : 0.0f;
                    cz = cz > 0.0f ? cz : 0.0f;
                    int near = cx * cx + cy * cy + cz * cz < w->lod_dist * w->lod_dist;
                    if (near != ((b->flags & WB_LOD_NEAR) != 0)) continue;
                }
                float dx = b->center[0] - eye[0], dy = b->center[1] - eye[1], dz = b->center[2] - eye[2];
                float lim = ((b->flags & WB_DECOR) ? w->decor_dist : far2) + b->radius;
                if (dx * dx + dy * dy + dz * dz > lim * lim) continue;
                if ((b->flags & WB_DECOR) && b->radius < 1.2f) {   /* as world_draw: props touching the camera */
                    float nr = b->radius + 0.6f;
                    if (dx * dx + dy * dy + dz * dz < nr * nr) continue;
                }
            }
            if (!sphere_visible(b->center, b->radius, 6)) continue;
            rw->vis[n++] = (u16)i;
        }
        flush(rw, &ds, n, st);
    }
    /* 3. objects with a mesh (fog walls last: additive, no depth write) */
    for (int pass = 0; pass < 2; pass++) {
        if (pass == 1) {
            sceGuEnable(GU_BLEND);
            sceGuBlendFunc(GU_ADD, GU_FIX, GU_FIX, 0xff707070, 0xffffffff);
            sceGuDepthMask(GU_TRUE);
            sceGuDisable(GU_FOG);
        }
        for (u32 i = 0; i < w->z->n_obj; i++) {
            const ZoneObj *o = &w->obj[i];
            if (o->mesh < 0 || !w->obj_on[i]) continue;
            if ((o->kind == WK_FOG) != (pass == 1)) continue;
            if (!mask_hit(o->mask, w->disp)) continue;
            const WorldMesh *m = &w->mesh[o->mesh];
            const float *M = w->obj_m[i];
            if (!sphere_visible(M + 9, m->radius, 6)) continue;
            for (u32 k = 0; k < m->batch_count; k++) draw_batch(rw, &ds, m->batch_start + k, M, M + 9, st);
            if (st) st->objs++;
        }
    }
    sceGuDisable(GU_BLEND);
    sceGuDepthMask(GU_FALSE);
    sceGuEnable(GU_FOG);
    sceGuDisable(GU_ALPHA_TEST);
    sceGuEnable(GU_CULL_FACE);
    sceGuTexScale(1.0f, 1.0f);
    sceGuTexOffset(0.0f, 0.0f);
    sceGuTexFilter(GU_LINEAR, GU_LINEAR);
    sceGuTexMode(GU_PSM_T8, 0, 0, 1);   /* later binds (characters, HUD) set level 0 only: no stale maxmips */
    {
        ScePspFMatrix4 id;
        memset(&id, 0, sizeof(id));
        id.x.x = id.y.y = id.z.z = id.w.w = 1.0f;
        sceGuSetMatrix(GU_MODEL, &id);
    }
    return st ? st->tris : 0;
}
