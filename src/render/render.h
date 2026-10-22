/*
 * Ash render module (docs/render.md): the "looks cool in the emulator, still fits the PSP" layer on top of the
 * plain sceGu drawing of src/game, src/world, src/viewer. Every effect has a flag in g_render.flags and a cost
 * estimate in docs/render.md §7; all of them are optional and independent.
 *
 *   RF_FOG        zone fog (sceGuFog) with DS1 FogBank colour/range + distance cull helper   (free on the GE)
 *   RF_MIPS       CLUT mip chains from tools/texmip.py (.ashmip), sceGuTexLevelMode           (RAM/VRAM +33 %)
 *   RF_LIGHTPASS  second pass: projected top-down lightmap (tools/cook_light.py .ashl), additive, only the
 *                 <=2500 brightest triangles near torches, palette flicker                     (tris + fill)
 *   RF_CHARLIGHT  GE lights on characters: DS1 LightBank directions (weak ones dropped) + nearest torch
 *   RF_RIM        rim light on characters: second skinned pass, view-space sphere map, additive (expensive)
 *   RF_SHADOWS    blob shadows (3x3 grid quad draped on the floor via a floor-height callback)
 *   RF_FX         sprite particles: torch/bonfire fire + glow, hit sparks + flash, roll dust
 *   RF_VLIT       (no runtime code) the caller draws the vertex-lit .ashm from cook_light --vertex-out
 *
 * Usage per frame (docs/render.md §8 has the exact patch for src/game/main.c):
 *   render_begin_frame(&view, &proj, eye, dt)         inside sceGuStart, after the camera, before the map
 *   render_tex_bind(...)                              instead of the sceGuClutLoad/TexMode/TexImage blocks
 *   <draw map>  then  render_light_pass_ashm(...)     (render_light_pass_begin/_draw/_end for other formats)
 *   render_char_light_begin(chest) ; chr_draw ; render_char_light_end() ; render_char_rim(chr, skin, 1)
 *   render_shadow_blob(...) per character ; fx_hit_spark / fx_roll_dust on events ; render_fx_draw()
 *   render_end_frame()
 * All state the module changes is restored to what src/game sets up (fog on, depth test on + write, cull on,
 * blend off, texture 2D on, GU_TEXTURE_COORDS, lighting off).
 */
#ifndef ASH_RENDER_H
#define ASH_RENDER_H

#include <psptypes.h>
#include <pspgu.h>

#define RF_FOG 0x01
#define RF_MIPS 0x02
#define RF_LIGHTPASS 0x04
#define RF_CHARLIGHT 0x08
#define RF_RIM 0x10
#define RF_SHADOWS 0x20
#define RF_FX 0x40
#define RF_VLIT 0x80   /* not a runtime switch: the caller draws the vertex-lit .ashm (cook_light --vertex-out) */
#define RF_ALL 0xff
/* recommended for the PSP (docs/render.md §7): everything except the rim pass and the light pass, which go
   on only in hero places (boss arena, bonfire) after the hardware day */
#define RF_PSP_DEFAULT (RF_FOG | RF_MIPS | RF_CHARLIGHT | RF_SHADOWS | RF_FX | RF_VLIT)
#define RF_DEBUG_COVER 0x100   /* passes write white (logic op SET): post.py counts their visible pixels */
#define RF_DEBUG_LM 0x200      /* light pass shows the lightmap itself (no blend) */

/* ------------------------------------------------------------------ zone look (.ashl, tools/cook_light.py) */

typedef struct {
    char magic[4];
    u32 version, n_light, n_sub, n_idx, lm_w, lm_h, flags;
    u32 off_light, off_sub, off_idx, off_clut, off_pix, pad[3];
    float lm_x0, lm_z0, lm_sx, lm_sz;
    float fog_near, fog_far;
    u32 fog_color, clear_color;
    float chl_dir[3][3];
    u32 chl_col[3], chl_ambient;
    float lm_gain;
    u32 pad2[2];
} AshlHdr;   /* 160 B */

typedef struct { float pos[3], radius; u32 color; u16 kind, flags; float strength, phase; } AshlLight;
typedef struct { u32 batch, idx_start, idx_count, pad; } AshlSub;

enum { RL_TORCH, RL_BONFIRE, RL_OTHER };

typedef struct {
    u8 *file;
    const AshlHdr *hdr;
    const AshlLight *light;
    const AshlSub *sub;
    const u16 *idx;
    const u32 *clut;        /* baked palette (index 0 = black = no light) */
    const u8 *pix;
    u32 __attribute__((aligned(16))) clut_anim[256];   /* flickered copy uploaded each frame */
} RenderZone;

/* .ashl buffer (64-aligned, kept by the caller). 0 = ok. */
int render_zone_load(RenderZone *z, u8 *file, int size);

/* ------------------------------------------------------------------ global state */

typedef struct {
    float near, far;        /* linear fog, metres from the eye */
    u32 color;              /* ABGR, fog */
    u32 clear;              /* ABGR, clear colour (backdrop behind everything) */
} RenderFog;

typedef struct {
    float dir[3][3];        /* GE directional lights 0-2: unit vectors TOWARDS the light, world space */
    u32 col[3];
    u32 ambient;
    float point_gain;       /* light 3 = nearest zone light as a point light, 0 = off */
} RenderCharLight;

typedef struct {
    u32 flags;              /* RF_* */
    RenderFog fog;
    RenderCharLight chl;
    float mip_bias;         /* sceGuTexLevelMode bias, +ve = blurrier (default 0) */
    int mip_filter;         /* GU_LINEAR_MIPMAP_LINEAR (default) or GU_LINEAR_MIPMAP_NEAREST (cheaper) */
    float shadow_alpha;     /* 0..1 darkness in the centre of a blob (default 0.55) */
    const RenderZone *zone; /* lights for torches/char light/particles, lightmap for RF_LIGHTPASS */
    /* per-frame counters (cost model, docs/render.md §7) */
    struct {
        int light_tris, light_draws, rim_tris, shadow_tris, fx_sprites, fx_px, shadow_px, light_px;
    } st;
} RenderState;

extern RenderState g_render;

/* Defaults: everything off, "old look" fog 30..110 dark blue-grey, the old single character light.
   render_zone_apply() then copies fog + char light from an .ashl into g_render (call after a zone loads). */
void render_init(void);
void render_zone_apply(const RenderZone *z);

/* Frame: stores view/proj/eye (sprites, rim, lightmap projection need them), advances fx and flicker by dt,
   sets clear colour + fog (RF_FOG) and resets counters. Call inside sceGuStart, before drawing the map. */
void render_begin_frame(const ScePspFMatrix4 *view, const ScePspFMatrix4 *proj, const float eye[3], float dt);
void render_end_frame(void);

/* ------------------------------------------------------------------ fog + cull */

/* sceGuFog + GU_FOG on (or off without RF_FOG). Already done by render_begin_frame. */
void render_fog_apply(void);
/* 1 if a bounding sphere is closer than the fog's far plane (beyond = fully fogged, skip the batch). */
int render_fog_visible(float cx, float cy, float cz, float radius);

/* ------------------------------------------------------------------ textures + mips (.ashmip, tools/texmip.py) */

typedef struct {
    u16 n_levels, psm;
    u16 w[8], h[8];
    u32 off[7];
} AmipEntry;

typedef struct {
    u8 *file;
    u32 n_tex;
    const AmipEntry *e;
} RenderMips;

int render_mips_load(RenderMips *m, u8 *file, int size);

/* Binds a CLUT texture (T4/T8, swizzled, CLUT ABGR8888) with its mip chain (tex index into `mips`, or mips=NULL /
   -1 = level 0 only). Sets ClutMode/ClutLoad/TexMode/TexImage(levels)/TexLevelMode/TexFilter. */
void render_tex_bind(int psm, int w, int h, const void *clut, const void *pix, const RenderMips *mp, int tex);

/* ------------------------------------------------------------------ second light pass */

/* For an .ashm scene (src/game/scene.h layout: Vertex 24 B = f32 uv, u32 colour, f32 xyz; batches 36 B).
   batch_vert_start(i) is read from the .ashm batch table: pass the batch array base + stride. */
void render_light_pass_ashm(const void *vert_base, const void *batch_table, int batch_stride);

/* Generic: one draw of the zone's subset `sub` over a vertex buffer with any vtype. pos_off/pos_scale = the
   batch's model transform for 16-bit vertices (src/world WorldBatch), NULL for float world-space vertices. */
void render_light_pass_draw(int sub, int vtype, const void *verts, const float pos_off[3], const float pos_scale[3]);
/* begin/end around render_light_pass_draw calls (blend, texture projection, fog colour) */
void render_light_pass_begin(void);
void render_light_pass_end(void);

/* ------------------------------------------------------------------ characters */

/* GE lighting for a character standing at pos (nearest zone light -> point light 3). Without RF_CHARLIGHT it
   sets the old single light. Enables GU_LIGHTING; the caller draws (chr_draw) and then calls _end. */
void render_char_light_begin(const float pos[3]);
void render_char_light_end(void);

/* Rim pass for a skinned mesh drawn again by the caller between _begin and _end with the SAME model matrix and
   bones; the caller must not bind its own textures in between (src/render/render_chr.c does it for Chr). */
void render_rim_begin(void);
void render_rim_end(void);

/* ------------------------------------------------------------------ blob shadows */

/* floor height under (x, z) between y_top and y_bot -> 1 + *h, or 0 (pcol_floor wrapper) */
typedef int (*RenderFloorFn)(void *ctx, float x, float z, float y_top, float y_bot, float *h);

/* Dark soft disc under a character: feet at pos, radius in metres. floor = NULL -> flat at pos[1]. */
void render_shadow_blob(const float pos[3], float radius, RenderFloorFn floor, void *ctx);

/* ------------------------------------------------------------------ particles (fx.c) */

void fx_reset(void);
/* continuous emitters: every zone light of kind torch/bonfire burns (call once after render_zone_apply) */
void fx_zone_fires(const RenderZone *z);
void fx_add_fire(const float pos[3], float size, int bonfire);
void fx_hit_spark(const float pos[3], const float dir[3], int parry);   /* sparks + flash */
void fx_roll_dust(const float pos[3], float yaw);                       /* dust puff at the feet */
void fx_update(float dt);           /* called by render_begin_frame */
void render_fx_draw(void);          /* after opaque + shadows, before the HUD */

#endif
