/*
 * Zone world: one `zone_<map>.pak` (tools/cook_world.py, byte layout in tools/ashw.py, docs/world.md) =
 *   "world" geometry (.ashw): chunked map batches with MSB draw-group masks, sky, object meshes, CLUT4/8 textures,
 *            12-byte vertices (u16 uv, RGB565 baked light, s16 xyz; per-batch scale/offset);
 *   "pcol"   Havok map collision, format v1 of src/game/pcol.h (pass world_pcol() to pcol_load());
 *   "ents"   zone entities (.ashz): collisions with display groups, objects, characters, regions with roles.
 *
 * Visibility = DS1's own draw groups (plan §3.4): a batch is drawn when batch.mask & disp != 0, disp = display groups
 * of the collision part the player stands on (world_set_cell_by_part with the pcol floor triangle's part), then a
 * frustum test on the batch sphere and a distance test against far_dist (hidden by fog). No BSP/PVS.
 *
 * All positions are PSP space (x mirrored vs FromSoft), like the rest of Ash. CPU float, no VFPU.
 */
#ifndef ASH_WORLD_H
#define ASH_WORLD_H

#include <psptypes.h>
#include <pspgu.h>

/* batch flags (tools/ashw.py BF_*) */
#define WB_ALPHA 1
#define WB_NOCULL 2
#define WB_SKY 4
#define WB_FAR 8
#define WB_MESH 16
#define WB_ADD 32
#define WB_LOD_NEAR 64   /* chunk LOD: drawn while the chunk cell (box) is within lod_dist (tessellated, finer) */
#define WB_LOD_FAR 128   /* chunk LOD: drawn beyond lod_dist (coarse) */
#define WB_DECOR 256     /* small static props: culled beyond decor_dist */
#define WB_BLEND 512     /* sky clouds: alpha-blended (CLUT with alpha levels), drawn after the rest of the sky */

/* object kinds (ashw.KINDS) */
enum { WK_DECOR, WK_BONFIRE, WK_DOOR, WK_CELL_DOOR, WK_GATE, WK_FOG, WK_LADDER, WK_FLOOR_BREAK, WK_BOULDER, WK_NEST,
       WK_BREAKABLE };
/* region roles (ashw.ROLES) */
enum { WR_NONE, WR_START, WR_RESPAWN, WR_BOSS_ARENA, WR_BOSS_START, WR_BOSS_ENTRY, WR_DEMON_DROP, WR_DEMON_LAND,
       WR_PLUNGE_ZONE, WR_BOSS_ESCAPE, WR_EXIT_CROW, WR_CROW_NEST, WR_MESSAGE, WR_TORCH, WR_FLOOR_COLLAPSE,
       WR_BOULDER_TRIGGER, WR_PATROL, WR_BGM, WR_TREASURE, WR_FOG_VFX, WR_KEY_DROP, WR_LIGHT, WR_OTHER,
       WR_DEMON_BALCONY, WR_PLUNGE_BALCONY_IN, WR_CELL, WR_INNER_DOOR };
enum { WS_POINT, WS_SPHERE, WS_CYLINDER, WS_BOX };   /* region shapes; box = w, h, d, bottom-centred [версия] */
#define WC_BOSS 1   /* character flags */
#define WC_CORE 2   /* suggested subset for slice S1 */

typedef struct {
    char magic[4];
    u32 version, n_tex, n_batch, n_mesh, n_vert, n_idx, flags;
    u32 off_tex, off_batch, off_mesh, off_vert, off_idx, pad[3];
    float bmin[3], bmax[3], chunk, pad2;
} WorldHdr;
typedef struct { u16 w, h, psm, flags; u32 off_clut, off_pix; } WorldTex;
typedef struct {
    u16 tex, flags;
    u32 vert_start, vert_count, idx_start, idx_count;
    float pos_off[3], pos_scale[3], uv_off[2], uv_scale[2], center[3], radius;
    u32 mask[4];
    u32 chunk;   /* (ix+512) | (iy+512)<<10 | (iz+512)<<20, LOD reference = centre of that grid cell */
} WorldBatch;
typedef struct { u32 batch_start, batch_count; float radius; u32 pad; } WorldMesh;
typedef struct { u16 u, v, c; s16 x, y, z; } WorldVert;

typedef struct {
    char magic[4];
    u32 version, n_col, n_obj, n_box, n_chr, n_reg, start_reg;
    u32 off_col, off_obj, off_box, off_chr, off_reg, pad[3];
} ZoneHdr;
typedef struct { char name[16]; u32 disp[4], draw[4]; s32 cam_row, sound; float reflect_y; u32 pad; } ZoneCol;
typedef struct {
    char name[16];
    u32 entity;
    u16 kind;
    s16 mesh;
    u16 box_start, box_count;
    u32 flags;
    float rot[9];   /* row-major */
    float pos[3];
    u32 mask[4];
} ZoneObj;
typedef struct { float c[3], h[3]; u32 flags, pad; } ZoneBox;   /* object-local box */
typedef struct {
    char model[8];
    u32 entity;
    float pos[3], yaw;   /* yaw: same convention as the game's player yaw (sceGumRotateY), = -MSB rot.y */
    s32 npc_param, think_param, talk_id;
    s16 patrol[2];       /* region index or -1 */
    u16 flags, pad;
    u32 mask[4];
} ZoneChr;
typedef struct {
    u32 entity;
    u16 shape, role;
    float pos[3], yaw, size[3];
    s32 param;           /* MESSAGE: FMG text id; TORCH/FOG_VFX: vfx id; TREASURE: item lot; RESPAWN: bonfire entity */
    u32 pad[2];
} ZoneReg;

typedef struct {
    int tris, batches, tex_changes, tested, objs;
} WorldStats;

typedef struct {
    u8 *pak;
    /* world */
    const WorldHdr *w;
    const WorldTex *tex;
    const WorldBatch *batch;
    const WorldMesh *mesh;
    const WorldVert *vert;
    const u16 *idx;
    const void **tpix;      /* per texture: pixels / CLUT address (pak blob, or VRAM after world_upload_vram) */
    const void **tclut;
    /* collision blob for pcol_load() */
    u8 *pcol;
    int pcol_size;
    s16 *part_col;          /* pcol part index -> ZoneCol index, -1 */
    int n_part;
    /* entities */
    const ZoneHdr *z;
    const ZoneCol *col;
    const ZoneObj *obj;
    const ZoneBox *box;
    const ZoneChr *chr;
    const ZoneReg *reg;
    u8 *obj_on;             /* per object: 1 = drawn + collides (fog walls, doors...) */
    float (*obj_m)[12];     /* per object: rot 3x3 row-major + pos, editable (doors swing, gate lifts) */
    /* view state */
    int cur_col;            /* ZoneCol index the visibility comes from, -1 = everything */
    u32 disp[4];
    float far_dist;         /* batches beyond this (minus radius) are skipped; put fog_far <= far_dist */
    float lod_dist;         /* chunk LOD switch distance (eye to the chunk grid cell box) */
    float decor_dist;       /* small props (WB_DECOR) culled beyond this */
    float fog_near, fog_far;
    u32 fog_color;
} World;

/* Loads the whole package (buffer from world_read_file or any 64-aligned buffer the World keeps). 0 = ok. */
int world_load(World *w, u8 *pak, int size);
/* Reads a whole file (cwd, host0:/, ms0:/ash/, umd0:/, disc0:/) into a 64-aligned buffer. Returns size or -1. */
int world_read_file(const char *name, u8 **out);
/* Moves textures + CLUTs into VRAM (vram = uncached-or-cached GE address, bytes free there). Returns bytes used,
   0 if they do not fit (textures stay in RAM, still works). */
int world_upload_vram(World *w, void *vram, int bytes);

/* Visibility cell. part = pcol part index of the floor triangle (Pcol.tri[t].part). Keeps the previous cell when
   part < 0 (in the air). world_set_cell(-1) = draw everything (debug / free camera). */
void world_set_cell_by_part(World *w, int part);
void world_set_cell(World *w, int zone_col);
int world_find_col(const World *w, const char *name);

/* Draws sky, world, enabled objects, fog walls. proj/view = what the game uses this frame (also loaded into GE).
   Leaves GU_MODEL = identity, fog/blend/depth-mask state restored to: fog on, depth test on, depth write on. */
int world_draw(World *w, const ScePspFMatrix4 *proj, const ScePspFMatrix4 *view, const float eye[3], WorldStats *st);

/* entities */
int world_find_region(const World *w, int role, int nth);     /* index into w->reg or -1 */
int world_find_obj(const World *w, u32 entity);               /* index into w->obj or -1 */
int world_find_chr(const World *w, u32 entity);
void world_obj_enable(World *w, int obj, int on);
/* point-in-region test (box: yaw-rotated, bottom-centred; sphere; cylinder; point = radius 1 m) */
int world_in_region(const World *w, int reg, const float p[3]);
/* Pushes a vertical cylinder (feet pos, radius, height) out of the boxes of enabled objects (doors, cell doors,
   gate, fog walls, ...). Returns how many boxes it touched. Call after pcol_move. */
int world_obj_push(World *w, float pos[3], float radius, float height);
/* Same, but keeps the side the cylinder was on at prev (its position before this step): a centre carried into or
   through a thin box (door leaf) by a fast step goes back out the face it entered, not out the far side. */
int world_obj_push_from(World *w, float pos[3], const float prev[3], float radius, float height);
/* Camera: free fraction (0..1) of a sphere of radius r swept a -> b against the boxes of enabled, solid objects
   (doors, gates, floor covers; not fog walls). Boxes the start already overlaps are ignored. */
float world_obj_cast(const World *w, const float a[3], const float b[3], float r);
/* Camera: free fraction of a segment a -> b (minus r) against the triangles of visible decor props (radius >=
   0.5 m: columns, rubble) that have no map collision. */
float world_decor_cast(const World *w, const float a[3], const float b[3], float r);
/* Highest top of an enabled object box under (x, z) between y_top and y_bottom (e.g. the breakable floor). */
int world_obj_floor(const World *w, float x, float z, float y_top, float y_bottom, float *h);

#endif
