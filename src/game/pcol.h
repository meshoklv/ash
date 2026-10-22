/*
 * Static map collision `.pcol` (tools/cook_collision.py: Havok hi-res collision of the map, triangles + uniform XZ
 * grid) and the queries the game needs (plan §3.7, the controls design notes §4.5):
 *   - capsule character controller: wall slide (spheres along the capsule, horizontal push-out), floor snap by a
 *     downward ray (walkable = normal.y >= slope_cos), steps up to `step` m, gravity when there is no floor;
 *   - ray cast (line of sight, floor probe);
 *   - sphere cast for the camera (sampled every r along the segment + bisection, so a zero-thickness wall cannot be
 *     skipped; triangles the start sphere already touches are ignored).
 * CPU float, no VFPU. All in PSP space (x mirrored vs FromSoft).
 */
#ifndef ASH_PCOL_H
#define ASH_PCOL_H

#include <psptypes.h>

typedef struct {
    char magic[4];
    u32 version, n_vert, n_tri, n_part, nx, nz, n_list;
    float x0, z0, cell, pad;
    u32 off_vert, off_tri, off_part, off_cell, off_list, pad2[3];
} PcolHdr;

typedef struct { u16 v[3], material; u8 part, flags; } __attribute__((packed)) PcolTri;
typedef struct { char name[16]; s32 cam_row; u32 pad; } PcolPart;
typedef struct { u32 start, count; } PcolCell;

typedef struct {
    u8 *file;
    const PcolHdr *hdr;
    const float *vert;          /* xyz */
    const PcolTri *tri;
    const PcolPart *part;
    const PcolCell *cell;
    const u16 *list;
    float *plane;               /* n_tri * (nx, ny, nz, d), computed at load */
    u16 *stamp;                 /* per-triangle query stamp (dedup across cells) */
    u16 cur_stamp;
    u8 part_off[256];           /* 1 = part ignored by every query (event-switched collision, e.g. a closed gate) */
    /* stats of the last frame, for the overlay */
    int tests;
    int overflow;               /* candidates dropped because of MAX_CAND (should stay 0) */
} Pcol;

typedef struct {
    float radius;               /* capsule radius */
    float height;               /* feet to top */
    float step;                 /* max step-up */
    float slope_cos;            /* walkable if normal.y >= this */
    float snap_down;            /* follow the floor down while grounded (stairs, slopes) */
    float gravity;
} PcolCapsule;

int pcol_load(Pcol *c, u8 *file, int size);

/* Segment o -> o + d (t in 0..1). Returns triangle index or -1; *t = hit fraction. walkable_only: ny >= slope_cos */
int pcol_raycast(Pcol *c, const float o[3], const float d[3], float *t, float min_ny);

/* Highest walkable floor under (x, z) between y_top and y_bottom. Returns triangle or -1. */
int pcol_floor(Pcol *c, float x, float z, float y_top, float y_bottom, float min_ny, float *h);

/* Moves the capsule (feet position) by (dx, dz): wall slide + steps + floor snap / fall. vy = vertical speed (m/s).
   Returns 1 if grounded. *floor_tri = triangle under the feet or -1. */
int pcol_move(Pcol *c, const PcolCapsule *cap, float pos[3], float dx, float dz, float *vy, float dt, int *floor_tri);

/* Sphere of radius r from a to b: returns the free fraction 0..1 (1 = no hit). */
float pcol_sphere_cast(Pcol *c, const float a[3], const float b[3], float r);

/* Event-switched collision (EMEVD "EnableMapCollision"): part index by name, on/off. */
int pcol_part_find(const Pcol *c, const char *name);
void pcol_part_enable(Pcol *c, int part, int on);

/* LockCamParam row of the part a triangle belongs to (MSB camera_1_id; -1 = default). */
int pcol_cam_row(const Pcol *c, int tri);

#endif
