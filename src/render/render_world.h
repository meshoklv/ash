/*
 * Fast zone drawing for the PSP (the perf-night design notes): a drop-in for src/world world_draw() over the same World.
 * Nothing in the .pak changes; everything is prepared once at load from the World (+ optional .ashmip):
 *
 *   RW_STRIPS  every batch's triangle list -> one stitched GU_TRIANGLE_STRIP (degenerate joins). The GE has no
 *              post-transform vertex cache [версия: PSP homebrew lore + PPSSPP, no public hardware test], so a list
 *              transforms 3 vertices per triangle, a strip ~1.2-1.6;
 *   RW_CACHE   static display lists: per batch a ready GE command block (model matrix with the batch's s16
 *              scale/offset, UV scale/offset, vertex type, buffers, PRIM, RET) built once; a frame CALLs it
 *              (2 command words emitted by the CPU instead of ~25 + sceGu bookkeeping);
 *   RW_SORT    visible opaque batches sorted by texture/state (fewer CLUT loads and texture changes);
 *   RW_MIPS    CLUT mip chains (.ashmip, tools/texmip.py zone_*.pak) with GU_LINEAR_MIPMAP_*;
 *   render_world_vram()  "hot" textures (+ their mips and CLUTs) into VRAM by use, until the space runs out
 *              (world_upload_vram() is all-or-nothing and has no mips).
 *
 * Visibility and drawing order are the same as world_draw (draw groups, LOD by chunk box, distance/decor culls, sky
 * passes, objects, fog walls), and so is the GE state it leaves behind. Memory: strips ~1.4 B x 3 per triangle of
 * the zone (m18_01: ~0.4 MB), call lists 100 B per batch (~0.23 MB), sort arrays 6 B per batch.
 */
#ifndef ASH_RENDER_WORLD_H
#define ASH_RENDER_WORLD_H

#include <psptypes.h>
#include <pspgu.h>
#include "world.h"
#include "render.h"

#define RW_STRIPS 1
#define RW_CACHE 2
#define RW_SORT 4
#define RW_MIPS 8
#define RW_ALL 15

typedef struct {
    World *w;
    int flags;
    /* strips */
    u16 *sidx;            /* all batches' strip indices */
    u32 *s_start;         /* per batch: start in sidx, 0xffffffff = keep the original list */
    u16 *s_count;
    /* display list cache */
    u32 *dl;
    u32 *dl_off;          /* per batch: word offset into dl, 0xffffffff = none (objects: matrix per frame) */
    /* textures: level pointers (pak / .ashmip / VRAM after render_world_vram) */
    u8 *n_lv;             /* extra mip levels per texture */
    const void *(*lv)[8]; /* per texture: level 0..7 pixels */
    u16 (*lw)[8], (*lh)[8];
    /* per-frame scratch */
    u16 *vis;
    u32 *key;
    /* stats of the preparation */
    int tris, list_idx, strip_idx, n_strip_batches, dl_bytes, strip_bytes;
} RenderWorld;

/* After world_load (and after sceGuInit). mp = NULL or the zone's .ashmip (RW_MIPS uses it). 0 = ok. */
int render_world_init(RenderWorld *rw, World *w, const RenderMips *mp, int flags);
/* Texture levels + CLUTs into VRAM, most used first (triangles of non-sky batches per byte), as far as `bytes`
   allows; the rest stays in RAM. Call after render_world_init, instead of world_upload_vram. Returns bytes used. */
int render_world_vram(RenderWorld *rw, void *vram, int bytes);
/* Same contract as world_draw(). */
int render_world_draw(RenderWorld *rw, const ScePspFMatrix4 *proj, const ScePspFMatrix4 *view, const float eye[3],
                      WorldStats *st);
void render_world_free(RenderWorld *rw);

/* Stitched strip of one triangle list (indices < 65535). out must hold 3 * n_idx entries (worst case). Returns the
   strip length (0 = empty). Used for characters too (render_chrf.c). */
int render_stripify(const u16 *idx, int n_idx, u16 *out);
/* 1 if a strip of n_strip indices is cheaper on the GE than the list of n_idx (vertex cost vert_ns each). */
int render_strip_better(int n_idx, int n_strip, float vert_ns);

#endif
