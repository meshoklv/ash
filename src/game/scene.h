/* Static `.ashm` scene (tools/cook_scene.py), same renderer as src/viewer: CLUT8 textures, baked vertex light,
 * per-batch sphere frustum culling. */
#ifndef ASH_SCENE_H
#define ASH_SCENE_H

#include <psptypes.h>
#include <pspgu.h>

typedef struct {
    char magic[4];
    u32 version, n_tex, n_batch, n_vert, n_idx, n_cam, flags;
    u32 off_tex, off_batch, off_vert, off_idx, off_cam;
    float bbox_min[3], pad;
} AshmHeader;

typedef struct { u16 w, h, psm, flags; u32 off_clut, off_pix; } AshmTex;
typedef struct { u16 tex, flags; u32 vert_start, vert_count, idx_start, idx_count; float cx, cy, cz, radius; } AshmBatch;
typedef struct { float u, v; u32 color; float x, y, z; } AshmVert;
typedef struct { float eye[3], target[3], fov, pad; } AshmCam;

typedef struct {
    u8 *file;
    AshmHeader *hdr;
    AshmTex *tex;
    AshmBatch *batch;
    AshmVert *vert;
    u16 *idx;
    AshmCam *cam;
} Scene;

int scene_load(Scene *s, u8 *file, int size);
/* vp = projection * view (for culling). Returns triangles drawn; *batches = draw calls. */
int scene_draw(const Scene *s, const ScePspFMatrix4 *vp, int *batches);

/* Reads a whole file (cwd, umd0:/, host0:/, ms0:/ash/, disc0:/) into a 64-byte aligned buffer. */
int ash_load_file(const char *name, u8 **out, char *found, int found_len);
void ash_mat_mul(ScePspFMatrix4 *out, const ScePspFMatrix4 *a, const ScePspFMatrix4 *b);

#endif
