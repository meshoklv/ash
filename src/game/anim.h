/*
 * Skeletal animation runtime for `.ashanim` (tools/cook_knight.py):
 *   sampling at 30 Hz keys (s16 quaternions, nlerp between keys), 2-clip crossfade, root motion track,
 *   hierarchy -> skin matrices (world * inverse bind) for GE hardware skinning.
 * Matrix products have a VFPU path and a plain C path (anim_set_vfpu); the VFPU one is checked against C at start.
 */
#ifndef ASH_ANIM_H
#define ASH_ANIM_H

#include <psptypes.h>
#include <pspgu.h>

#define ANIM_MAX_BONES 32

typedef struct {
    char magic[4];
    u32 version, n_bones, n_clips;
    float t_scale, fps;
    u32 off_clips, pad;
} AnimFileHdr;

typedef struct {
    char name[16];
    u32 anim_id;
    u16 n_frames, flags; /* flags: 1 = loop */
    u32 off_rot, off_tstart, off_tflag, off_trans, off_root, n_trans;
    float root_dist;
    u32 pad[3];
} AnimClip;

#define CLIP_LOOP 1

typedef struct {
    float q[4]; /* x y z w */
    float t[3];
    float pad;
} BonePose;

typedef struct {
    u8 *file;
    AnimFileHdr *hdr;
    AnimClip *clips;
    int n_bones;
} AnimSet;

int anim_load(AnimSet *set, u8 *file, int size);
int anim_find(const AnimSet *set, const char *name);
float anim_duration(const AnimSet *set, int clip); /* seconds, (n_frames - 1) / fps */

/* Local pose of every bone at time t (seconds; wrapped for loops, clamped otherwise). */
void anim_sample(const AnimSet *set, int clip, float t, BonePose *out);
/* out = nlerp(a, b, w), w = 0 -> a. out may alias a. */
void anim_blend(int n_bones, const BonePose *a, const BonePose *b, float w, BonePose *out);
/* Root motion between t0 and t1 (t1 >= t0, loops wrap): dx, dy, dz, dyaw in character space. */
void anim_root_delta(const AnimSet *set, int clip, float t0, float t1, float out[4]);

/* Local poses -> skin matrices skin[k] = world[k] * inv_bind[k]. parent[k] < k. Matrices 16-byte aligned. */
void anim_skin_matrices(int n_bones, const int *parent, const BonePose *local, const ScePspFMatrix4 *inv_bind,
                        ScePspFMatrix4 *world, ScePspFMatrix4 *skin);

void anim_set_vfpu(int on);
int anim_vfpu(void);
/* Compares VFPU and C matrix products; returns max abs difference (and disables VFPU if > 1e-3). */
float anim_selftest_vfpu(void);

#endif
