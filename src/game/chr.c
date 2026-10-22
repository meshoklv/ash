#include "chr.h"

#include <malloc.h>
#include <pspgum.h>
#include <pspkernel.h>
#include <string.h>

_Static_assert(sizeof(ChrHdr) == 64, "ChrHdr");
_Static_assert(sizeof(ChrBone) == 80, "ChrBone");
_Static_assert(sizeof(ChrChunk) == 32, "ChrChunk");
_Static_assert(sizeof(ChrVert) == 32, "ChrVert");

int chr_load(Chr *c, u8 *file, int size)
{
    if (size < (int)sizeof(ChrHdr)) return -1;
    ChrHdr *h = (ChrHdr *)file;
    if (memcmp(h->magic, "ASHC", 4) != 0 || h->version != 1 || h->n_bone > 32) return -2;
    c->file = file;
    c->hdr = h;
    c->bones = (ChrBone *)(file + h->off_bone);
    c->tex = (ChrTex *)(file + h->off_tex);
    c->chunks = (ChrChunk *)(file + h->off_chunk);
    c->verts = (ChrVert *)(file + h->off_vert);
    c->idx = (u16 *)(file + h->off_idx);
    c->inv_bind = memalign(16, sizeof(ScePspFMatrix4) * h->n_bone);
    if (!c->inv_bind) return -3;
    for (u32 k = 0; k < h->n_bone; k++) {
        const float *s = c->bones[k].inv_bind;
        float *d = (float *)&c->inv_bind[k];
        for (int col = 0; col < 4; col++) {
            d[col * 4 + 0] = s[col * 3 + 0];
            d[col * 4 + 1] = s[col * 3 + 1];
            d[col * 4 + 2] = s[col * 3 + 2];
            d[col * 4 + 3] = col == 3 ? 1.0f : 0.0f;
        }
        c->parent[k] = c->bones[k].parent;
    }
    sceKernelDcacheWritebackAll();
    return 0;
}

int chr_bone(const Chr *c, const char *name)
{
    for (u32 k = 0; k < c->hdr->n_bone; k++)
        if (strncmp(c->bones[k].name, name, 28) == 0) return (int)k;
    return -1;
}

int chr_draw(const Chr *c, const ScePspFMatrix4 *skin)
{
    int cur_tex = -1, cur_alpha = -1, tris = 0;
    for (u32 i = 0; i < c->hdr->n_chunk; i++) {
        const ChrChunk *ch = &c->chunks[i];
        if (ch->tex != cur_tex) {
            const ChrTex *t = &c->tex[ch->tex];
            sceGuClutMode(GU_PSM_8888, 0, 0xff, 0);
            sceGuClutLoad(256 / 8, c->file + t->off_clut);
            sceGuTexMode(GU_PSM_T8, 0, 0, 1);
            sceGuTexImage(0, t->w, t->h, t->w, c->file + t->off_pix);
            sceGuTexFlush();
            cur_tex = ch->tex;
        }
        int alpha = ch->flags & 1;
        if (alpha != cur_alpha) {
            if (alpha) { sceGuEnable(GU_ALPHA_TEST); sceGuDisable(GU_CULL_FACE); }
            else { sceGuDisable(GU_ALPHA_TEST); sceGuEnable(GU_CULL_FACE); }
            cur_alpha = alpha;
        }
        /* unused slots get a valid matrix too: weight 0 * stale/NaN register would still poison the sum */
        for (int b = 0; b < 8; b++) sceGuBoneMatrix(b, &skin[ch->pal[b < ch->n_pal ? b : 0]]);
        sceGumDrawArray(GU_TRIANGLES, CHR_VTYPE, ch->idx_count, c->idx + ch->idx_start, c->verts + ch->vert_start);
        tris += ch->idx_count / 3;
    }
    sceGuDisable(GU_ALPHA_TEST);
    sceGuEnable(GU_CULL_FACE);
    return tris;
}
