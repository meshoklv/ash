/*
 * GE display list walker + PSP-3000 GE cost model (docs/psp-cost-model.md).
 *
 * Follows the list like the GE does (CALL/RET/JUMP/BASE/OFFSET/ORIGIN), keeps the state that matters for cost
 * (vertex type, matrices, viewport, scissor, enables, texture, CLUT), and for every PRIM:
 *   - vertex stage: count, bytes fetched, skinning weights, lighting;
 *   - (coverage=1) transforms the vertices on the CPU (bones -> world -> view -> proj -> viewport), clips each
 *     triangle to the near plane and the scissor, back-face culls, and sums covered pixels (overdraw included,
 *     depth-rejected pixels included - the GE still reads Z for them) and texel bytes per pixel (minification);
 *   - cost = prim + max(vertex, pixel) + overlap * min(vertex, pixel), coefficients from psp_cost.cfg.
 * Command opcodes and vertex layout follow PPSSPP GPU/ge_constants.h and VertexDecoder (sha 400a97f).
 */
#include "ge_scan.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

typedef unsigned int u32;
typedef unsigned short u16;
typedef unsigned char u8;

int g_ge_log_draws = 0;
int g_ge_coverage = 1;
int g_ge_list_seq = 0;

void ge_stats_clear(GeStats *s) { memset(s, 0, sizeof(*s)); }

void ge_stats_add(GeStats *a, const GeStats *b)
{
    a->lists += b->lists; a->cmds += b->cmds; a->prims += b->prims; a->prims_skinned += b->prims_skinned;
    a->verts += b->verts;
    for (int i = 0; i < VF_N; i++) a->verts_fmt[i] += b->verts_fmt[i];
    a->verts_skinned += b->verts_skinned; a->verts_lit += b->verts_lit;
    a->tris += b->tris; a->tris_culled += b->tris_culled; a->tris_clipped += b->tris_clipped; a->sprites += b->sprites;
    a->tex_binds += b->tex_binds; a->tex_binds_ram += b->tex_binds_ram; a->clut_loads += b->clut_loads;
    a->clut_bytes += b->clut_bytes; a->bone_mats += b->bone_mats;
    if (b->max_weights > a->max_weights) a->max_weights = b->max_weights;
    if (b->max_bones_draw > a->max_bones_draw) a->max_bones_draw = b->max_bones_draw;
    a->tex_over512 += b->tex_over512; a->tex_nonclut += b->tex_nonclut; a->tex_unswizzled += b->tex_unswizzled;
    a->unreadable += b->unreadable;
    a->vbytes += b->vbytes; a->ibytes += b->ibytes;
    a->px += b->px; a->px_tex += b->px_tex; a->px_zr += b->px_zr; a->px_zw += b->px_zw; a->px_blend += b->px_blend;
    a->px_clear += b->px_clear; a->px_tex_ram += b->px_tex_ram;
    a->tex_bytes_ram += b->tex_bytes_ram; a->tex_bytes_vram += b->tex_bytes_vram;
    if (b->minif_max > a->minif_max) a->minif_max = b->minif_max;
    a->tex_ratio_sum += b->tex_ratio_sum;
    a->ns_list += b->ns_list; a->ns_cmd += b->ns_cmd; a->ns_vert += b->ns_vert; a->ns_pix += b->ns_pix;
    a->ns_state += b->ns_state; a->ns_total += b->ns_total;
}

/* ------------------------------------------------------------------ GE state (persists across lists, like the GE) */
static struct {
    u32 base, offset, vaddr, iaddr, vtype;
    float world[12], view[12], proj[16], bone[96];
    int wi, vi, pi, bi;
    float vsx, vsy, vcx, vcy, offx, offy;
    int sx1, sy1, sx2, sy2;
    float tsu, tsv, tou, tov;
    int lte, le[4], cull_en, cull_mode, tme, abe, zte, zmask, clear, fb32;
    u32 tex0, tbw0, tsize0, tpsm, tmode, tflt, clut, clutu;
    u32 tmin, tlvl, texl[8], tbwl[8];   /* min filter, TexLevelMode, mip level addresses (1..7) */
    u32 last_tex_key;
} G = {
    .world = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0},
    .view = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0},
    .proj = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1},
    .vsx = 240, .vsy = -136, .vcx = 2048, .vcy = 2048, .offx = 2048 - 240, .offy = 2048 - 136,
    .sx2 = 479, .sy2 = 271, .tsu = 1, .tsv = 1,
};

static float f24(u32 op)
{
    union { u32 i; float f; } u;
    u.i = (op & 0x00FFFFFF) << 8;
    return u.f;
}

/* GE address -> CPU pointer (NULL if outside user RAM / VRAM) */
static const void *gptr(u32 a)
{
    a &= 0x1FFFFFFF;
    if (a >= 0x08800000 && a < 0x0C000000) return (const void *)a;
    if ((a & 0x0F000000) == 0x04000000) return (const void *)(0x44000000 | (a & 0x001FFFFF));
    return NULL;
}

static u32 rel_addr(u32 op) { return ((((G.base & 0x000F0000) << 8) | (op & 0x00FFFFFF)) + G.offset) & 0x0FFFFFFF; }

/* ------------------------------------------------------------------ vertex decoder */
typedef struct {
    int size, nw, wfmt, woff, tfmt, toff, cfmt, pfmt, poff, nfmt, idx, through;
} VDec;

static void vdec_init(VDec *d, u32 vt)
{
    int off = 0, maxal = 1, sz;
    memset(d, 0, sizeof(*d));
    d->tfmt = vt & 3; d->cfmt = (vt >> 2) & 7; d->nfmt = (vt >> 5) & 3; d->pfmt = (vt >> 7) & 3;
    d->wfmt = (vt >> 9) & 3; d->idx = (vt >> 11) & 3; d->through = (vt >> 23) & 1;
    d->nw = d->wfmt ? (int)((vt >> 14) & 7) + 1 : 0;
#define AL(a) do { off = (off + (a) - 1) & ~((a) - 1); if ((a) > maxal) maxal = (a); } while (0)
    if (d->wfmt) { sz = d->wfmt == 1 ? 1 : d->wfmt == 2 ? 2 : 4; AL(sz); d->woff = off; off += sz * d->nw; }
    if (d->tfmt) { sz = d->tfmt == 1 ? 1 : d->tfmt == 2 ? 2 : 4; AL(sz); d->toff = off; off += 2 * sz; }
    if (d->cfmt) { sz = d->cfmt == 7 ? 4 : 2; AL(sz); off += sz; }
    if (d->nfmt) { sz = d->nfmt == 1 ? 1 : d->nfmt == 2 ? 2 : 4; AL(sz); off += 3 * sz; }
    if (d->pfmt) { sz = d->pfmt == 1 ? 1 : d->pfmt == 2 ? 2 : 4; AL(sz); d->poff = off; off += 3 * sz; }
    AL(maxal);
#undef AL
    d->size = off * (int)(((vt >> 18) & 7) + 1);   /* morph: N copies, we read the first */
}

typedef struct { float X, Y, Z, W, u, v; } TV;   /* clip space + texel coordinates */

static int g_tw = 1, g_th = 1;

static void m43(const float *m, const float *p, float *o)
{
    o[0] = m[0] * p[0] + m[3] * p[1] + m[6] * p[2] + m[9];
    o[1] = m[1] * p[0] + m[4] * p[1] + m[7] * p[2] + m[10];
    o[2] = m[2] * p[0] + m[5] * p[1] + m[8] * p[2] + m[11];
}

static void vfetch(const VDec *d, const u8 *v, TV *o)
{
    float p[3], q[3], r[3];
    const u8 *pp = v + d->poff;
    if (d->through) {
        if (d->pfmt == 3) { const float *f = (const float *)pp; p[0] = f[0]; p[1] = f[1]; p[2] = f[2]; }
        else if (d->pfmt == 2) { const short *s = (const short *)pp; p[0] = s[0]; p[1] = s[1]; p[2] = (u16)s[2]; }
        else { const signed char *s = (const signed char *)pp; p[0] = s[0]; p[1] = s[1]; p[2] = s[2]; }
        o->X = p[0]; o->Y = p[1]; o->Z = 0; o->W = 1;
        if (d->tfmt == 3) { const float *f = (const float *)(v + d->toff); o->u = f[0]; o->v = f[1]; }
        else if (d->tfmt == 2) { const u16 *s = (const u16 *)(v + d->toff); o->u = s[0]; o->v = s[1]; }
        else if (d->tfmt == 1) { o->u = v[d->toff]; o->v = v[d->toff + 1]; }
        else { o->u = o->v = 0; }
        return;
    }
    if (d->pfmt == 3) { const float *f = (const float *)pp; p[0] = f[0]; p[1] = f[1]; p[2] = f[2]; }
    else if (d->pfmt == 2) { const short *s = (const short *)pp; p[0] = s[0] / 32768.0f; p[1] = s[1] / 32768.0f; p[2] = s[2] / 32768.0f; }
    else { const signed char *s = (const signed char *)pp; p[0] = s[0] / 128.0f; p[1] = s[1] / 128.0f; p[2] = s[2] / 128.0f; }
    if (d->nw) {
        float acc[3] = {0, 0, 0};
        for (int i = 0; i < d->nw && i < 8; i++) {
            float w;
            if (d->wfmt == 1) w = v[d->woff + i] / 128.0f;
            else if (d->wfmt == 2) w = ((const u16 *)(v + d->woff))[i] / 32768.0f;
            else w = ((const float *)(v + d->woff))[i];
            if (w == 0.0f) continue;
            m43(G.bone + 12 * i, p, q);
            acc[0] += w * q[0]; acc[1] += w * q[1]; acc[2] += w * q[2];
        }
        p[0] = acc[0]; p[1] = acc[1]; p[2] = acc[2];
    }
    m43(G.world, p, q);
    m43(G.view, q, r);
    const float *P = G.proj;
    o->X = P[0] * r[0] + P[4] * r[1] + P[8] * r[2] + P[12];
    o->Y = P[1] * r[0] + P[5] * r[1] + P[9] * r[2] + P[13];
    o->Z = P[2] * r[0] + P[6] * r[1] + P[10] * r[2] + P[14];
    o->W = P[3] * r[0] + P[7] * r[1] + P[11] * r[2] + P[15];
    float u = 0, vv = 0;
    if (d->tfmt == 3) { const float *f = (const float *)(v + d->toff); u = f[0]; vv = f[1]; }
    else if (d->tfmt == 2) { const u16 *s = (const u16 *)(v + d->toff); u = s[0] / 32768.0f; vv = s[1] / 32768.0f; }
    else if (d->tfmt == 1) { u = v[d->toff] / 128.0f; vv = v[d->toff + 1] / 128.0f; }
    o->u = (u * G.tsu + G.tou) * g_tw;
    o->v = (vv * G.tsv + G.tov) * g_th;
}

/* ------------------------------------------------------------------ triangle coverage */
typedef struct { double px, tex_bytes, tex_bytes_ram, ratio_sum; int tris, culled, clipped; double minif; } Acc;
static float g_bpp;      /* texel bytes of the bound texture (0 = no texture sampling) */
static float g_bilin;    /* bilinear multiplier of the bound texture */

/* texel bytes fetched per covered pixel: bpp x texels/pixel x bilinear, capped at one cache line (thrash) */
static float tex_bytes_lvl(float ratio)
{
    if (ratio < PC(ge_tex_ratio_min)) ratio = PC(ge_tex_ratio_min);
    float b = g_bpp * ratio * g_bilin;
    return b < PC(ge_tex_line_bytes) ? b : PC(ge_tex_line_bytes);
}

/* Mip chains (TexMode maxmips > 0, TexLevelMode AUTO): level = floor(0.5*log2(texels per pixel) + bias), clamped
   to the chain; the texel area per pixel drops 4x per level. *_MIPMAP_LINEAR min filters read the next level too.
   [оценка: the hardware LOD formula is PPSSPP's; no mip timing on the real GE is public]. Without mips = level 0. */
static int g_maxmips, g_mip_lin, g_lvl_auto;
static float g_lvl_bias;
static u32 g_lvl_ram;    /* bit L = level L is in main RAM */
static float tex_bytes_px(float ratio, double *ram_part)
{
    int L = 0;
    if (g_maxmips > 0 && g_lvl_auto && ratio > 1.0f) {
        float lod = 0.5f * log2f(ratio) + g_lvl_bias;
        L = lod > 0 ? (int)lod : 0;
        if (L > g_maxmips) L = g_maxmips;
    } else if (g_maxmips > 0 && g_lvl_auto && g_lvl_bias >= 1.0f) {
        L = (int)g_lvl_bias;
        if (L > g_maxmips) L = g_maxmips;
    }
    float r = ratio;
    for (int i = 0; i < L; i++) r *= 0.25f;
    float b = tex_bytes_lvl(r), br = (g_lvl_ram >> L) & 1 ? b : 0;
    if (g_maxmips > 0 && g_mip_lin && L < g_maxmips) {
        float b2 = tex_bytes_lvl(r * 0.25f);
        b += b2;
        if ((g_lvl_ram >> (L + 1)) & 1) br += b2;
    }
    *ram_part = br;
    return b;
}
static int g_cull_flip;

static float poly_area(const float *x, const float *y, int n)
{
    float a = 0;
    for (int i = 0; i < n; i++) {
        int j = i + 1 == n ? 0 : i + 1;
        a += x[i] * y[j] - x[j] * y[i];
    }
    return 0.5f * a;
}

/* clip polygon (x,y) against x >= lo (axis 0) etc.; returns new count */
static int clip_axis(float *x, float *y, int n, int axis, float lim, int keep_greater)
{
    float ox[12], oy[12];
    int m = 0;
    for (int i = 0; i < n; i++) {
        int j = i + 1 == n ? 0 : i + 1;
        float ci = axis ? y[i] : x[i], cj = axis ? y[j] : x[j];
        int ini = keep_greater ? ci >= lim : ci <= lim, inj = keep_greater ? cj >= lim : cj <= lim;
        if (ini && m < 12) { ox[m] = x[i]; oy[m] = y[i]; m++; }
        if (ini != inj && m < 12) {
            float t = (lim - ci) / (cj - ci);
            ox[m] = x[i] + (x[j] - x[i]) * t; oy[m] = y[i] + (y[j] - y[i]) * t; m++;
        }
    }
    memcpy(x, ox, m * sizeof(float)); memcpy(y, oy, m * sizeof(float));
    return m;
}

static void tri(const TV *a, const TV *b, const TV *c, int through, Acc *acc)
{
    acc->tris++;
    if (!g_ge_coverage) return;
    const TV *v[3] = {a, b, c};
    float cx[4], cy[4], cz[4], cw[4];
    int n = 0, inside = 0;
    float sx[12] = {0}, sy[12] = {0};
    if (through) {
        for (int i = 0; i < 3; i++) { sx[i] = v[i]->X; sy[i] = v[i]->Y; }
        n = 3; inside = 3;
    } else {
        for (int i = 0; i < 3; i++) inside += (v[i]->Z + v[i]->W >= 0.0f && v[i]->W > 1e-6f);
        if (inside == 0) { acc->clipped++; return; }
        if (inside == 3) {
            for (int i = 0; i < 3; i++) { float iw = 1.0f / v[i]->W; sx[i] = v[i]->X * iw; sy[i] = v[i]->Y * iw; }
            n = 3;
        } else {   /* near-plane clip (Z + W >= 0) in clip space */
            for (int i = 0; i < 3; i++) {
                int j = i == 2 ? 0 : i + 1;
                float di = v[i]->Z + v[i]->W, dj = v[j]->Z + v[j]->W;
                if (di >= 0 && v[i]->W > 1e-6f) { cx[n] = v[i]->X; cy[n] = v[i]->Y; cz[n] = v[i]->Z; cw[n] = v[i]->W; n++; }
                if ((di >= 0) != (dj >= 0) && n < 4) {
                    float t = di / (di - dj);
                    cx[n] = v[i]->X + (v[j]->X - v[i]->X) * t; cy[n] = v[i]->Y + (v[j]->Y - v[i]->Y) * t;
                    cz[n] = v[i]->Z + (v[j]->Z - v[i]->Z) * t; cw[n] = v[i]->W + (v[j]->W - v[i]->W) * t; n++;
                }
            }
            for (int i = 0; i < n; i++) {
                float iw = cw[i] > 1e-6f ? 1.0f / cw[i] : 1e6f;
                sx[i] = cx[i] * iw; sy[i] = cy[i] * iw;
            }
            (void)cz;
        }
        for (int i = 0; i < n; i++) {
            sx[i] = sx[i] * G.vsx + G.vcx - G.offx;
            sy[i] = sy[i] * G.vsy + G.vcy - G.offy;
        }
    }
    float area = poly_area(sx, sy, n);
    if (G.cull_en && !through) {
        /* CULL=1 keeps triangles with positive screen area (y down), CULL=0 the negative ones: PPSSPP software
         * TransformUnit::SendTriangle + Rasterizer edge functions [проверено по коду PPSSPP 400a97f] */
        int keep = ((area < 0) ^ (G.cull_mode & 1) ^ g_cull_flip) ? 1 : 0;
        if (!keep) { acc->culled++; return; }
    }
    float full = fabsf(area);
    n = clip_axis(sx, sy, n, 0, (float)G.sx1, 1);
    n = clip_axis(sx, sy, n, 0, (float)G.sx2 + 1, 0);
    n = clip_axis(sx, sy, n, 1, (float)G.sy1, 1);
    n = clip_axis(sx, sy, n, 1, (float)G.sy2 + 1, 0);
    if (n < 3) return;
    float cov = fabsf(poly_area(sx, sy, n));
    acc->px += cov;
    if (g_bpp > 0) {
        float ratio = 1.0f;
        if (inside == 3 && full > 0.01f) {
            float uva = fabsf((b->u - a->u) * (c->v - a->v) - (c->u - a->u) * (b->v - a->v)) * 0.5f;
            ratio = uva / full;
        }
        if (ratio > acc->minif) acc->minif = ratio;
        acc->ratio_sum += cov * (ratio < 64.0f ? ratio : 64.0f);
        double rp;
        acc->tex_bytes += cov * tex_bytes_px(ratio, &rp);
        acc->tex_bytes_ram += cov * rp;
    }
}

static void sprite(const TV *a, const TV *b, int through, Acc *acc)
{
    acc->tris++;
    if (!g_ge_coverage) return;
    float x0 = a->X, y0 = a->Y, x1 = b->X, y1 = b->Y;
    if (!through) {
        if (a->W <= 1e-6f || b->W <= 1e-6f) { acc->clipped++; return; }
        x0 = a->X / a->W * G.vsx + G.vcx - G.offx; y0 = a->Y / a->W * G.vsy + G.vcy - G.offy;
        x1 = b->X / b->W * G.vsx + G.vcx - G.offx; y1 = b->Y / b->W * G.vsy + G.vcy - G.offy;
    }
    float full = fabsf((x1 - x0) * (y1 - y0));
    float lx = fminf(x0, x1), hx = fmaxf(x0, x1), ly = fminf(y0, y1), hy = fmaxf(y0, y1);
    lx = fmaxf(lx, (float)G.sx1); ly = fmaxf(ly, (float)G.sy1);
    hx = fminf(hx, (float)G.sx2 + 1); hy = fminf(hy, (float)G.sy2 + 1);
    if (hx <= lx || hy <= ly) return;
    float cov = (hx - lx) * (hy - ly);
    acc->px += cov;
    if (g_bpp > 0) {
        float ratio = full > 0.01f ? fabsf((b->u - a->u) * (b->v - a->v)) / full : 1.0f;
        if (ratio > acc->minif) acc->minif = ratio;
        acc->ratio_sum += cov * (ratio < 64.0f ? ratio : 64.0f);
        double rp;
        acc->tex_bytes += cov * tex_bytes_px(ratio, &rp);
        acc->tex_bytes_ram += cov * rp;
    }
}

/* ------------------------------------------------------------------ PRIM */
#define VCACHE 4096
static TV g_vc[VCACHE];

static float tex_bpp(u32 psm)
{
    static const float b[11] = {2, 2, 2, 4, 0.5f, 1, 2, 4, 0.5f, 1, 1};
    return psm < 11 ? b[psm] : 2;
}

static void do_prim(u32 op, GeStats *s)
{
    int prim = (op >> 16) & 7, count = op & 0xFFFF;
    if (count == 0) return;
    VDec d;
    vdec_init(&d, G.vtype);
    int isz = d.idx == 1 ? 1 : d.idx == 2 ? 2 : d.idx == 3 ? 4 : 0;
    int lit = !d.through && G.lte && d.nfmt;
    int nl = lit ? G.le[0] + G.le[1] + G.le[2] + G.le[3] : 0;
    int vf = d.through ? VF_THROUGH : d.pfmt == 1 ? VF_S8 : d.pfmt == 2 ? VF_S16 : VF_FLOAT;

    s->prims++;
    s->verts += count;
    s->verts_fmt[vf] += count;
    if (d.nw) { s->prims_skinned++; s->verts_skinned += count; }
    if (lit) s->verts_lit += count;
    if (d.nw > s->max_weights) s->max_weights = d.nw;
    if (d.nw > s->max_bones_draw) s->max_bones_draw = d.nw;
    double vbytes = (double)count * d.size, ibytes = (double)count * isz;
    s->vbytes += vbytes; s->ibytes += ibytes;

    /* texture actually sampled? */
    int texon = G.tme && !G.clear && d.tfmt;
    u32 taddr = (G.tex0 & 0xFFFFF0) | ((G.tbw0 << 8) & 0x0F000000);
    int tram = (taddr & 0x0F000000) != 0x04000000;
    g_tw = 1 << (G.tsize0 & 0xF); g_th = 1 << ((G.tsize0 >> 8) & 0xF);
    double ns_state = 0;
    if (texon) {
        u32 key = taddr ^ (G.tsize0 << 3) ^ (G.tpsm << 28) ^ (G.clut << 1) ^ G.tmode;
        if (key != G.last_tex_key) {
            G.last_tex_key = key;
            s->tex_binds++;
            if (tram) s->tex_binds_ram++;
            ns_state += PC(ge_tex_bind_ns);
            if (g_tw > (int)PC(budget_tex_max) || g_th > (int)PC(budget_tex_max)) s->tex_over512++;
            if (G.tpsm < 4 || G.tpsm > 7) s->tex_nonclut++;
            if (!(G.tmode & 1)) s->tex_unswizzled++;
        }
    }
    g_bpp = texon ? tex_bpp(G.tpsm) : 0;
    g_bilin = (G.tflt & 1) ? PC(ge_tex_bilinear_mul) : 1.0f;   /* mag filter linear */
    g_maxmips = (int)((G.tmode >> 16) & 7);
    g_mip_lin = G.tmin == 6 || G.tmin == 7;          /* GU_NEAREST/LINEAR_MIPMAP_LINEAR */
    if (G.tmin < 4) g_maxmips = 0;                    /* min filter without mipmaps samples level 0 only */
    g_lvl_auto = (G.tlvl & 3) == 0;
    g_lvl_bias = (float)(signed char)((G.tlvl >> 16) & 0xFF) / 16.0f;
    g_lvl_ram = tram ? 1u : 0u;
    for (int l = 1; l <= g_maxmips; l++) {
        u32 la = (G.texl[l] & 0xFFFFF0) | ((G.tbwl[l] << 8) & 0x0F000000);
        if ((la & 0x0F000000) != 0x04000000) g_lvl_ram |= 1u << l;
    }

    /* coverage */
    Acc acc;
    memset(&acc, 0, sizeof(acc));
    const u8 *vb = gptr(G.vaddr);
    const u8 *ib = isz ? gptr(G.iaddr) : NULL;
    int readable = vb && (!isz || ib) && prim >= 3;
    if (!readable && prim >= 3) s->unreadable++;
    if (readable && g_ge_coverage) {
        int lo = 0, hi = count - 1;
        if (isz) {
            lo = 0x7FFFFFFF; hi = -1;
            for (int i = 0; i < count; i++) {
                int k = isz == 1 ? ib[i] : isz == 2 ? ((const u16 *)ib)[i] : (int)((const u32 *)ib)[i];
                if (k < lo) lo = k;
                if (k > hi) hi = k;
            }
        }
        int cached = hi - lo + 1 <= VCACHE;
        if (cached)
            for (int k = lo; k <= hi; k++) vfetch(&d, vb + (u32)k * d.size, &g_vc[k - lo]);
        TV tmp[3];
#define VTX(i, slot) (cached ? &g_vc[(isz == 1 ? ib[i] : isz == 2 ? ((const u16 *)ib)[i] : (int)((const u32 *)ib)[i]) - lo] \
                             : (vfetch(&d, vb + (u32)(isz == 1 ? ib[i] : isz == 2 ? ((const u16 *)ib)[i] : (int)((const u32 *)ib)[i]) * d.size, &tmp[slot]), &tmp[slot]))
#define VTXN(i, slot) (cached ? &g_vc[(i) - lo + lo] : (vfetch(&d, vb + (u32)(i) * d.size, &tmp[slot]), &tmp[slot]))
        if (prim == 3) {
            for (int i = 0; i + 2 < count; i += 3) {
                const TV *a = isz ? VTX(i, 0) : VTXN(i, 0), *b = isz ? VTX(i + 1, 1) : VTXN(i + 1, 1), *c = isz ? VTX(i + 2, 2) : VTXN(i + 2, 2);
                tri(a, b, c, d.through, &acc);
            }
        } else if (prim == 4) {
            for (int i = 0; i + 2 < count; i++) {
                const TV *a = isz ? VTX(i, 0) : VTXN(i, 0), *b = isz ? VTX(i + 1, 1) : VTXN(i + 1, 1), *c = isz ? VTX(i + 2, 2) : VTXN(i + 2, 2);
                if (i & 1) tri(b, a, c, d.through, &acc); else tri(a, b, c, d.through, &acc);
            }
        } else if (prim == 5) {
            TV f0;
            const TV *p0 = isz ? VTX(0, 0) : VTXN(0, 0);
            f0 = *p0;
            for (int i = 1; i + 1 < count; i++) {
                const TV *b = isz ? VTX(i, 1) : VTXN(i, 1), *c = isz ? VTX(i + 1, 2) : VTXN(i + 1, 2);
                tri(&f0, b, c, d.through, &acc);
            }
        } else if (prim == 6) {
            for (int i = 0; i + 1 < count; i += 2) {
                const TV *a = isz ? VTX(i, 0) : VTXN(i, 0), *b = isz ? VTX(i + 1, 1) : VTXN(i + 1, 1);
                sprite(a, b, d.through, &acc);
                s->sprites++;
            }
        }
#undef VTX
#undef VTXN
    } else {
        acc.tris = prim == 3 ? count / 3 : (prim == 4 || prim == 5) ? count - 2 : prim == 6 ? count / 2 : 0;
        if (acc.tris < 0) acc.tris = 0;
    }
    s->tris += acc.tris; s->tris_culled += acc.culled; s->tris_clipped += acc.clipped;
    if (acc.minif > s->minif_max) s->minif_max = acc.minif;
    s->tex_ratio_sum += acc.ratio_sum;

    /* ---- cost */
    double tv = count * (PC(ge_vert_ns) + d.nw * PC(ge_weight_ns) + (lit ? PC(ge_light_ns) + nl * PC(ge_light_each_ns) : 0)) +
                (vbytes + ibytes) * PC(ge_vert_byte_ns) + acc.tris * PC(ge_tri_ns);
    double tp, zr = 0, zw = 0, bl = 0, tb_ram = 0, tb_vram = 0;
    if (G.clear) {
        tp = acc.px * PC(ge_clear_px_ns) * (G.fb32 ? PC(ge_pix_32bit_mul) : 1.0f);
        s->px_clear += acc.px;
    } else {
        zr = G.zte ? acc.px : 0;
        zw = (G.zte && !G.zmask) ? acc.px : 0;
        bl = G.abe ? acc.px : 0;
        double tb = acc.tex_bytes * (texon && !(G.tmode & 1) && prim != 6 ? PC(ge_tex_unswizzled_mul) : 1.0f);
        if (texon) {
            double mul = acc.tex_bytes > 0 ? tb / acc.tex_bytes : 1.0;
            tb_ram = acc.tex_bytes_ram * mul;
            tb_vram = tb - tb_ram;
        }
        tp = acc.px * PC(ge_pix_ns) * (G.fb32 ? PC(ge_pix_32bit_mul) : 1.0f) + zr * PC(ge_pix_zread_ns) +
             zw * PC(ge_pix_zwrite_ns) + bl * PC(ge_pix_blend_ns) + (texon ? acc.px * PC(ge_tex_px_ns) : 0) +
             tb_ram * PC(ge_tex_ram_byte_ns) * PC(ge_tex_ram_contention) + tb_vram * PC(ge_tex_vram_byte_ns);
        s->px += acc.px;
        if (texon) { s->px_tex += acc.px; if (tram) s->px_tex_ram += acc.px; }
        s->px_zr += zr; s->px_zw += zw; s->px_blend += bl;
        s->tex_bytes_ram += tb_ram; s->tex_bytes_vram += tb_vram;
    }
    double big = tv > tp ? tv : tp, small = tv > tp ? tp : tv;
    double t = PC(ge_prim_ns) + big + PC(ge_overlap) * small;
    s->ns_vert += tv; s->ns_pix += tp; s->ns_state += ns_state + PC(ge_prim_ns);
    s->ns_total += t + ns_state;
    if (g_ge_log_draws)
        printf("PSPEST-D l=%d prim=%d cnt=%d vsz=%d isz=%d nw=%d lit=%d nl=%d tris=%d culled=%d px=%.0f zr=%.0f zw=%.0f "
               "bl=%.0f tex=%d texpx=%.0f tbr=%.0f tbv=%.0f clear=%d fb32=%d bind=%.0f ns=%.0f\n",
               g_ge_list_seq, prim, count, d.size, isz, d.nw, lit, nl, acc.tris, acc.culled, acc.px, zr, zw, bl, texon,
               texon ? acc.px : 0.0, tb_ram, tb_vram, G.clear, G.fb32, ns_state, t + ns_state);

    /* the GE advances the pointers past what it read */
    if (isz) G.iaddr += count * isz; else G.vaddr += count * d.size;
}

/* ------------------------------------------------------------------ list walker */
void ge_scan_list(const void *list, int size_bytes, GeStats *s)
{
    u32 pc = (u32)list & 0x0FFFFFFF;
    u32 end = size_bytes > 0 ? pc + (u32)size_bytes : 0;
    u32 stack[32], ostack[32];
    int sp = 0, guard = 0;
    s->lists++;
    g_ge_list_seq++;
    g_cull_flip = (int)PC(cull_flip);
    s->ns_list += PC(ge_list_ns);
    s->ns_total += PC(ge_list_ns);
    for (;;) {
        if (++guard > 2000000) break;
        if (sp == 0 && end && pc >= end) break;
        const u32 *p = gptr(pc);
        if (!p) break;
        u32 op = *p, cmd = op >> 24;
        u32 cur = pc;
        pc += 4;
        s->cmds++;
        switch (cmd) {
        case 0x01: G.vaddr = rel_addr(op); break;
        case 0x02: G.iaddr = rel_addr(op); break;
        case 0x04: do_prim(op, s); break;
        case 0x05: case 0x06: { int u = op & 0xFF, v = (op >> 8) & 0xFF; s->verts += u * v; } break;   /* bezier/spline: count only */
        case 0x08: pc = rel_addr(op & 0x00FFFFFC); break;
        case 0x0A: if (sp < 32) { stack[sp] = pc; ostack[sp] = G.offset; sp++; } pc = rel_addr(op & 0x00FFFFFC); break;
        case 0x0B: if (sp > 0) { sp--; pc = stack[sp]; G.offset = ostack[sp]; } break;
        case 0x0C: goto done;                       /* END */
        case 0x10: G.base = op & 0x00FFFFFF; break;
        case 0x12: G.vtype = op & 0x00FFFFFF; break;
        case 0x13: G.offset = (op & 0x00FFFFFF) << 8; break;
        case 0x14: G.offset = cur; break;
        case 0x17: G.lte = op & 1; break;
        case 0x18: case 0x19: case 0x1A: case 0x1B: G.le[cmd - 0x18] = op & 1; break;
        case 0x1D: G.cull_en = op & 1; break;
        case 0x1E: G.tme = op & 1; break;
        case 0x21: G.abe = op & 1; break;
        case 0x23: G.zte = op & 1; break;
        case 0x2A: G.bi = op & 0x7F; break;
        case 0x2B: if (G.bi < 96) { G.bone[G.bi] = f24(op); if (G.bi % 12 == 11) s->bone_mats++; G.bi++; } break;
        case 0x3A: G.wi = op & 0xF; break;
        case 0x3B: if (G.wi < 12) G.world[G.wi++] = f24(op); break;
        case 0x3C: G.vi = op & 0xF; break;
        case 0x3D: if (G.vi < 12) G.view[G.vi++] = f24(op); break;
        case 0x3E: G.pi = op & 0xF; break;
        case 0x3F: if (G.pi < 16) G.proj[G.pi++] = f24(op); break;
        case 0x42: G.vsx = f24(op); break;
        case 0x43: G.vsy = f24(op); break;
        case 0x45: G.vcx = f24(op); break;
        case 0x46: G.vcy = f24(op); break;
        case 0x48: G.tsu = f24(op); break;
        case 0x49: G.tsv = f24(op); break;
        case 0x4A: G.tou = f24(op); break;
        case 0x4B: G.tov = f24(op); break;
        case 0x4C: G.offx = (op & 0xFFFF) / 16.0f; break;
        case 0x4D: G.offy = (op & 0xFFFF) / 16.0f; break;
        case 0x9B: G.cull_mode = op & 1; break;
        case 0xA0: G.tex0 = op & 0x00FFFFFF; break;
        case 0xA8: G.tbw0 = op & 0x00FFFFFF; break;
        case 0xB0: G.clut = op & 0x00FFFFFF; break;
        case 0xB1: G.clutu = op & 0x00FFFFFF; break;
        case 0xB8: G.tsize0 = op & 0x00FFFFFF; break;
        case 0xC2: G.tmode = op & 0x00FFFFFF; break;
        case 0xC3: G.tpsm = op & 0xF; break;
        case 0xC6: G.tflt = (op >> 8) & 7; G.tmin = op & 7; break;
        case 0xC8: G.tlvl = op & 0x00FFFFFF; break;
        case 0xA1: case 0xA2: case 0xA3: case 0xA4: case 0xA5: case 0xA6: case 0xA7: G.texl[cmd - 0xA0] = op & 0x00FFFFFF; break;
        case 0xA9: case 0xAA: case 0xAB: case 0xAC: case 0xAD: case 0xAE: case 0xAF: G.tbwl[cmd - 0xA8] = op & 0x00FFFFFF; break;
        case 0xC4: { int blocks = op & 0x3F; s->clut_loads++; s->clut_bytes += blocks * 32;
                     s->ns_state += blocks * 32 * PC(ge_clut_byte_ns); s->ns_total += blocks * 32 * PC(ge_clut_byte_ns);
                     G.last_tex_key = 0; } break;
        case 0xD2: G.fb32 = (op & 3) == 3; break;
        case 0xD3: G.clear = op & 1; break;
        case 0xD4: G.sx1 = op & 0x3FF; G.sy1 = (op >> 10) & 0x3FF; break;
        case 0xD5: G.sx2 = op & 0x3FF; G.sy2 = (op >> 10) & 0x3FF; break;
        case 0xE7: G.zmask = op & 1; break;
        default: break;
        }
    }
done:
    s->ns_cmd += PC(ge_cmd_ns) * guard;
    s->ns_total += PC(ge_cmd_ns) * guard;
    if (g_ge_log_draws) printf("PSPEST-L l=%d cmds=%d\n", g_ge_list_seq, guard);
}
