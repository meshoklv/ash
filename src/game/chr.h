/*
 * Skinned character mesh `.ashc` (tools/cook_knight.py), drawn with GE hardware skinning:
 * per chunk up to 8 sceGuBoneMatrix + GU_WEIGHTS(8) u8 weights (sum 128), GE lighting with 8-bit normals.
 */
#ifndef ASH_CHR_H
#define ASH_CHR_H

#include <psptypes.h>
#include <pspgu.h>

typedef struct {
    char magic[4];
    u32 version, n_bone, n_tex, n_chunk, n_vert, n_idx, flags;
    u32 off_bone, off_tex, off_chunk, off_vert, off_idx;
    float radius;
    u32 pad[2];
} ChrHdr;

typedef struct { s32 parent; char name[28]; float inv_bind[12]; } ChrBone;
typedef struct { u16 w, h, psm, flags; u32 off_clut, off_pix; } ChrTex;
typedef struct { u16 tex, flags; u8 n_pal, pad[3]; u8 pal[8]; u32 vert_start, vert_count, idx_start, idx_count; } ChrChunk;
typedef struct { u8 w[8]; float u, v; s8 nx, ny, nz, pad; float x, y, z; } ChrVert;

#define CHR_VTYPE (GU_WEIGHTS(8) | GU_WEIGHT_8BIT | GU_TEXTURE_32BITF | GU_NORMAL_8BIT | GU_VERTEX_32BITF | \
                   GU_INDEX_16BIT | GU_TRANSFORM_3D)

typedef struct {
    u8 *file;
    ChrHdr *hdr;
    ChrBone *bones;
    ChrTex *tex;
    ChrChunk *chunks;
    ChrVert *verts;
    u16 *idx;
    int parent[32];
    ScePspFMatrix4 *inv_bind; /* n_bone, 16-aligned, expanded to 4x4 */
} Chr;

int chr_load(Chr *c, u8 *file, int size);
int chr_bone(const Chr *c, const char *name);
/* Draws with the current GU_MODEL/VIEW/PROJECTION; skin = n_bone matrices. Returns triangles drawn. */
int chr_draw(const Chr *c, const ScePspFMatrix4 *skin);

#endif
