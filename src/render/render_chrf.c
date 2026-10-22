/* Compact skinned characters: s16 positions/UV, per-chunk weight count, strips (render_chrf.h, the perf-night design notes). */
#include "render_chrf.h"
#include "render_world.h"

#include <malloc.h>
#include <math.h>
#include <pspgum.h>
#include <pspkernel.h>
#include <stdlib.h>
#include <string.h>

static int al2(int x) { return (x + 1) & ~1; }

int render_chrf_init(ChrFast *f, const Chr *c)
{
    memset(f, 0, sizeof(*f));
    f->c = c;
    const ChrHdr *h = c->hdr;
    const int nc = (int)h->n_chunk, nv = (int)h->n_vert;
    /* bounds of the whole mesh */
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    float ulo[2] = {1e30f, 1e30f}, uhi[2] = {-1e30f, -1e30f};
    for (int i = 0; i < nv; i++) {
        const ChrVert *v = &c->verts[i];
        const float p[3] = {v->x, v->y, v->z}, uv[2] = {v->u, v->v};
        for (int k = 0; k < 3; k++) { if (p[k] < lo[k]) lo[k] = p[k]; if (p[k] > hi[k]) hi[k] = p[k]; }
        for (int k = 0; k < 2; k++) { if (uv[k] < ulo[k]) ulo[k] = uv[k]; if (uv[k] > uhi[k]) uhi[k] = uv[k]; }
    }
    if (nv == 0) return -1;
    float ext = 0;
    for (int k = 0; k < 3; k++) {
        f->off[k] = 0.5f * (lo[k] + hi[k]);
        float e = 0.5f * (hi[k] - lo[k]);
        if (e > ext) ext = e;
    }
    f->scale = (ext < 1.0f ? 1.0f : ext) * (32768.0f / 32767.0f);
    int n16 = f->scale > 2.0f;   /* normals are scaled by 1/scale (the bone matrices carry the scale): keep bits */
    for (int k = 0; k < 2; k++) {
        float r = uhi[k] - ulo[k];
        if (r < 1e-6f) r = 1.0f;
        f->uv_off[k] = ulo[k];
        f->uv_scale[k] = r * (32768.0f / 65535.0f);
    }
    f->ch = calloc(nc, sizeof(ChrfChunk));
    if (!f->ch) return -2;
    /* layout + sizes */
    int vbytes = 0, ibytes_max = 0;
    for (int i = 0; i < nc; i++) {
        const ChrChunk *s = &c->chunks[i];
        ChrfChunk *d = &f->ch[i];
        int nw = 1;
        for (u32 v = 0; v < s->vert_count; v++)
            for (int k = 7; k >= nw; k--)
                if (c->verts[s->vert_start + v].w[k]) { nw = k + 1; break; }
        if (nw < s->n_pal && nw < 1) nw = 1;
        d->nw = nw;
        d->n_pal = s->n_pal;
        memcpy(d->pal, s->pal, 8);
        d->tex = s->tex;
        d->flags = s->flags;
        int stride = al2(nw) + 4 + (n16 ? 6 : al2(3)) + 6;
        d->vtype = GU_WEIGHTS(nw) | GU_WEIGHT_8BIT | GU_TEXTURE_16BIT | (n16 ? GU_NORMAL_16BIT : GU_NORMAL_8BIT) |
                   GU_VERTEX_16BIT | GU_INDEX_16BIT | GU_TRANSFORM_3D;
        d->voff = vbytes;
        vbytes += stride * s->vert_count;
        vbytes = (vbytes + 15) & ~15;
        if ((int)s->idx_count * 2 > ibytes_max) ibytes_max = s->idx_count * 2;
        f->verts_in += s->idx_count;
        f->w_in += 8 * s->idx_count;
        f->w_out += nw * s->idx_count;
    }
    f->vbuf = memalign(16, vbytes + 16);
    u16 *tmp = malloc(sizeof(u16) * 3 * (ibytes_max / 2 + 4));
    int itotal = 0;
    for (int i = 0; i < nc; i++) itotal += c->chunks[i].idx_count + 8;
    f->ibuf = memalign(16, sizeof(u16) * itotal);
    if (!f->vbuf || !tmp || !f->ibuf) { free(tmp); return -3; }
    int iat = 0;
    for (int i = 0; i < nc; i++) {
        const ChrChunk *s = &c->chunks[i];
        ChrfChunk *d = &f->ch[i];
        int nw = d->nw, stride = al2(nw) + 4 + (n16 ? 6 : al2(3)) + 6;
        u8 *o = f->vbuf + d->voff;
        for (u32 v = 0; v < s->vert_count; v++, o += stride) {
            const ChrVert *sv = &c->verts[s->vert_start + v];
            int at = 0;
            memcpy(o, sv->w, nw);   /* weights: same bytes (sum 128), slots past nw are 0 */
            at = al2(nw);
            u16 *uv = (u16 *)(o + at);
            for (int k = 0; k < 2; k++) {
                float q = ((k ? sv->v : sv->u) - f->uv_off[k]) / (f->uv_scale[k] * (65535.0f / 32768.0f)) * 65535.0f;
                uv[k] = (u16)(q < 0 ? 0 : q > 65535.0f ? 65535 : (int)(q + 0.5f));
            }
            at += 4;
            const s8 nn[3] = {sv->nx, sv->ny, sv->nz};
            if (n16) {
                s16 *nr = (s16 *)(o + at);
                for (int k = 0; k < 3; k++) {
                    float q = nn[k] * 256.0f / f->scale;   /* s8/128 -> s16/32768, divided by the scale */
                    nr[k] = (s16)(q < -32767 ? -32767 : q > 32767 ? 32767 : (int)lrintf(q));
                }
                at += 6;
            } else {
                s8 *nr = (s8 *)(o + at);
                for (int k = 0; k < 3; k++) {
                    float q = nn[k] / f->scale;
                    nr[k] = (s8)(q < -127 ? -127 : q > 127 ? 127 : (int)lrintf(q));
                }
                o[at + 3] = 0;
                at += 4;
            }
            s16 *p = (s16 *)(o + at);
            const float pp[3] = {sv->x, sv->y, sv->z};
            for (int k = 0; k < 3; k++) {
                float q = (pp[k] - f->off[k]) / f->scale * 32768.0f;
                p[k] = (s16)(q < -32767 ? -32767 : q > 32767 ? 32767 : (int)lrintf(q));
            }
        }
        int n = render_stripify(c->idx + s->idx_start, s->idx_count, tmp);
        d->ioff = iat;
        if (render_strip_better(s->idx_count, n, 60.0f + 0.75f * (stride + 2) + 6.0f * nw)) {
            memcpy(f->ibuf + iat, tmp, n * 2);
            d->count = n; d->prim = GU_TRIANGLE_STRIP;
        } else {
            memcpy(f->ibuf + iat, c->idx + s->idx_start, s->idx_count * 2);
            d->count = s->idx_count; d->prim = GU_TRIANGLES;
        }
        iat += (d->count + 1) & ~1;
        f->verts_out += d->count;
        f->idx_in += s->idx_count;
        f->idx_out += d->count;
    }
    free(tmp);
    f->tpix = malloc(sizeof(void *) * (h->n_tex + 1));
    f->tclut = malloc(sizeof(void *) * (h->n_tex + 1));
    if (!f->tpix || !f->tclut) return -4;
    for (u32 t = 0; t < h->n_tex; t++) { f->tpix[t] = c->file + c->tex[t].off_pix; f->tclut[t] = c->file + c->tex[t].off_clut; }
    f->bytes = vbytes + iat * 2 + nc * (int)sizeof(ChrfChunk);
    sceKernelDcacheWritebackAll();
    return 0;
}

void render_chrf_free(ChrFast *f)
{
    free(f->vbuf); free(f->ibuf); free(f->ch); free(f->tpix); free(f->tclut);
    memset(f, 0, sizeof(*f));
}

int render_chrf_vram(ChrFast *f, void *vram, int bytes)
{
    const Chr *c = f->c;
    int need = 0;
    for (u32 t = 0; t < c->hdr->n_tex; t++) need += ((c->tex[t].w * c->tex[t].h + 15) & ~15) + 1024;
    if (need > bytes) return 0;
    u8 *d = (u8 *)vram;
    for (u32 t = 0; t < c->hdr->n_tex; t++) {
        int px = c->tex[t].w * c->tex[t].h;   /* CLUT8 */
        memcpy(d, f->tpix[t], px); f->tpix[t] = d; d += (px + 15) & ~15;
        memcpy(d, f->tclut[t], 1024); f->tclut[t] = d; d += 1024;
    }
    sceKernelDcacheWritebackAll();
    return need;
}

int render_chrf_draw(const ChrFast *f, const ScePspFMatrix4 *skin)
{
    const Chr *c = f->c;
    /* bone' = skin x T(off) x S(scale): the s16 positions decode to (q/32768) */
    static ScePspFMatrix4 bm[32] __attribute__((aligned(16)));
    const int nb = (int)c->hdr->n_bone;
    const float s = f->scale;
    for (int b = 0; b < nb && b < 32; b++) {
        const ScePspFMatrix4 *m = &skin[b];
        ScePspFMatrix4 *o = &bm[b];
        o->x.x = m->x.x * s; o->x.y = m->x.y * s; o->x.z = m->x.z * s; o->x.w = 0;
        o->y.x = m->y.x * s; o->y.y = m->y.y * s; o->y.z = m->y.z * s; o->y.w = 0;
        o->z.x = m->z.x * s; o->z.y = m->z.y * s; o->z.z = m->z.z * s; o->z.w = 0;
        o->w.x = m->x.x * f->off[0] + m->y.x * f->off[1] + m->z.x * f->off[2] + m->w.x;
        o->w.y = m->x.y * f->off[0] + m->y.y * f->off[1] + m->z.y * f->off[2] + m->w.y;
        o->w.z = m->x.z * f->off[0] + m->y.z * f->off[1] + m->z.z * f->off[2] + m->w.z;
        o->w.w = 1;
    }
    int cur_tex = -1, cur_alpha = -1, tris = 0;
    int slot[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
    sceGuTexScale(f->uv_scale[0], f->uv_scale[1]);
    sceGuTexOffset(f->uv_off[0], f->uv_off[1]);
    for (u32 i = 0; i < c->hdr->n_chunk; i++) {
        const ChrfChunk *ch = &f->ch[i];
        if (ch->tex != cur_tex) {
            const ChrTex *t = &c->tex[ch->tex];
            sceGuClutMode(GU_PSM_8888, 0, 0xff, 0);
            sceGuClutLoad(256 / 8, f->tclut[ch->tex]);
            sceGuTexMode(GU_PSM_T8, 0, 0, 1);
            sceGuTexImage(0, t->w, t->h, t->w, f->tpix[ch->tex]);
            sceGuTexFlush();
            cur_tex = ch->tex;
        }
        int alpha = ch->flags & 1;
        if (alpha != cur_alpha) {
            if (alpha) { sceGuEnable(GU_ALPHA_TEST); sceGuDisable(GU_CULL_FACE); }
            else { sceGuDisable(GU_ALPHA_TEST); sceGuEnable(GU_CULL_FACE); }
            cur_alpha = alpha;
        }
        for (int b = 0; b < ch->nw; b++) {
            int bone = ch->pal[b < ch->n_pal ? b : 0];
            if (slot[b] != bone) { sceGuBoneMatrix(b, &bm[bone]); slot[b] = bone; }
        }
        sceGumDrawArray(ch->prim, ch->vtype, ch->count, f->ibuf + ch->ioff, f->vbuf + ch->voff);
        tris += c->chunks[i].idx_count / 3;
    }
    sceGuDisable(GU_ALPHA_TEST);
    sceGuEnable(GU_CULL_FACE);
    sceGuTexScale(1.0f, 1.0f);
    sceGuTexOffset(0.0f, 0.0f);
    return tris;
}
