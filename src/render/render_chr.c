#include "render_chr.h"

#include <pspgum.h>

int render_char_rim(const Chr *c, const ScePspFMatrix4 *skin, int alpha_chunks)
{
    if (!(g_render.flags & RF_RIM)) return 0;
    int tris = 0;
    render_rim_begin();
    for (u32 i = 0; i < c->hdr->n_chunk; i++) {
        const ChrChunk *ch = &c->chunks[i];
        if ((ch->flags & 1) && !alpha_chunks) continue;
        for (int b = 0; b < 8; b++) sceGuBoneMatrix(b, &skin[ch->pal[b < ch->n_pal ? b : 0]]);
        sceGumDrawArray(GU_TRIANGLES, CHR_VTYPE, ch->idx_count, c->idx + ch->idx_start, c->verts + ch->vert_start);
        tris += ch->idx_count / 3;
    }
    render_rim_end();
    g_render.st.rim_tris += tris;
    return tris;
}
