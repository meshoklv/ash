/*
 * Compact skinned characters (the perf-night design notes): a drop-in for chr_draw() built at load from a loaded `.ashc` v1
 * (nothing in the file format or the cookers changes):
 *   - positions s16 with one offset + uniform scale per mesh (folded into the bone matrices at draw time),
 *   - UV u16 with one offset/scale per mesh (sceGuTexScale/Offset), normals s8 (s16 for meshes > 2 m half-extent),
 *   - per chunk only as many weights as the chunk has bones (GU_WEIGHTS(n_pal), was always 8),
 *   - per chunk one stitched triangle strip (render_stripify), bone matrices sent only when the slot changes,
 *   - textures optionally in VRAM (render_chrf_vram).
 * 32 B float vertices with 8 weights -> 16-20 B with 1-8 weights. The `.ashc` file stays loaded (the game still
 * reads bones from it); the compact copy is extra RAM: knight ~+60 KB, hollow ~+25 KB [проверено: init log].
 */
#ifndef ASH_RENDER_CHRF_H
#define ASH_RENDER_CHRF_H

#include <psptypes.h>
#include <pspgu.h>
#include "chr.h"

typedef struct {
    u32 voff, ioff, vtype;
    u16 count, prim, nw, tex, flags, n_pal;
    u8 pal[8];
} ChrfChunk;

typedef struct {
    const Chr *c;
    u8 *vbuf;
    u16 *ibuf;
    ChrfChunk *ch;
    const void **tpix, **tclut;   /* per texture (file or VRAM) */
    float off[3], scale;           /* position = q / 32768 * scale + off */
    float uv_off[2], uv_scale[2];  /* GE: u = q / 32768 * uv_scale + uv_off */
    int bytes, verts_in, verts_out, idx_in, idx_out, w_in, w_out;
} ChrFast;

/* After chr_load. 0 = ok. */
int render_chrf_init(ChrFast *f, const Chr *c);
/* Same contract as chr_draw (current GU_MODEL/VIEW/PROJECTION, skin = n_bone matrices). Returns triangles. */
int render_chrf_draw(const ChrFast *f, const ScePspFMatrix4 *skin);
/* Character textures + CLUTs into VRAM (all or nothing). Returns bytes used, 0 = stays in RAM. */
int render_chrf_vram(ChrFast *f, void *vram, int bytes);
void render_chrf_free(ChrFast *f);

#endif
