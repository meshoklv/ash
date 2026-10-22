#include "world.h"

#include <malloc.h>
#include <math.h>
#include <pspgum.h>
#include <pspiofilemgr.h>
#include <pspkernel.h>
#include <stdio.h>
#include <string.h>

#define VTYPE (GU_TEXTURE_16BIT | GU_COLOR_5650 | GU_VERTEX_16BIT | GU_INDEX_16BIT | GU_TRANSFORM_3D)

typedef struct { char magic[4]; u32 version, n; u32 pad; } PakHdr;
typedef struct { char name[16]; u32 off, size, pad[2]; } PakEnt;

/* .pcol v1 header (src/game/pcol.h, tools/cook_collision.py): only the part table is read here */
typedef struct {
    char magic[4];
    u32 version, n_vert, n_tri, n_part, nx, nz, n_list;
    float x0, z0, cell, pad;
    u32 off_vert, off_tri, off_part, off_cell, off_list, pad2[3];
} PcolHdrW;
typedef struct { char name[16]; s32 cam_row; u32 pad; } PcolPartW;

static const char *g_prefixes[] = {"", "host0:/", "ms0:/ash/", "umd0:/", "disc0:/", NULL};

int world_read_file(const char *name, u8 **out)
{
    char path[256];
    for (int i = 0; g_prefixes[i]; i++) {
        snprintf(path, sizeof(path), "%s%s", g_prefixes[i], name);
        SceUID fd = sceIoOpen(path, PSP_O_RDONLY, 0);
        if (fd < 0) continue;
        int size = (int)sceIoLseek(fd, 0, PSP_SEEK_END);
        sceIoLseek(fd, 0, PSP_SEEK_SET);
        u8 *buf = memalign(64, size + 64);
        if (!buf) { sceIoClose(fd); printf("world: out of memory for %s (%d B)\n", path, size); return -1; }
        int got = 0;
        while (got < size) {
            int r = sceIoRead(fd, buf + got, size - got > 1 << 20 ? 1 << 20 : size - got);
            if (r <= 0) break;
            got += r;
        }
        sceIoClose(fd);
        *out = buf;
        return got;
    }
    return -1;
}

static const PakEnt *pak_find(const u8 *pak, const char *name)
{
    const PakHdr *h = (const PakHdr *)pak;
    const PakEnt *e = (const PakEnt *)(pak + sizeof(PakHdr));
    for (u32 i = 0; i < h->n; i++)
        if (strncmp(e[i].name, name, 16) == 0) return &e[i];
    return NULL;
}

int world_load(World *w, u8 *pak, int size)
{
    memset(w, 0, sizeof(*w));
    if (size < 64 || memcmp(pak, "APAK", 4) != 0) return -1;
    w->pak = pak;
    const PakEnt *ew = pak_find(pak, "world"), *ec = pak_find(pak, "pcol"), *ez = pak_find(pak, "ents");
    if (!ew || !ez) return -2;
    u8 *wb = pak + ew->off;
    w->w = (const WorldHdr *)wb;
    if (memcmp(w->w->magic, "ASHW", 4) != 0 || w->w->version != 1) return -3;
    w->tex = (const WorldTex *)(wb + w->w->off_tex);
    w->batch = (const WorldBatch *)(wb + w->w->off_batch);
    w->mesh = (const WorldMesh *)(wb + w->w->off_mesh);
    w->vert = (const WorldVert *)(wb + w->w->off_vert);
    w->idx = (const u16 *)(wb + w->w->off_idx);
    w->tpix = malloc(sizeof(void *) * w->w->n_tex);
    w->tclut = malloc(sizeof(void *) * w->w->n_tex);
    for (u32 i = 0; i < w->w->n_tex; i++) {
        w->tpix[i] = wb + w->tex[i].off_pix;
        w->tclut[i] = wb + w->tex[i].off_clut;
    }
    u8 *zb = pak + ez->off;
    w->z = (const ZoneHdr *)zb;
    if (memcmp(w->z->magic, "ASHZ", 4) != 0 || w->z->version != 1) return -4;
    w->col = (const ZoneCol *)(zb + w->z->off_col);
    w->obj = (const ZoneObj *)(zb + w->z->off_obj);
    w->box = (const ZoneBox *)(zb + w->z->off_box);
    w->chr = (const ZoneChr *)(zb + w->z->off_chr);
    w->reg = (const ZoneReg *)(zb + w->z->off_reg);
    w->obj_on = malloc(w->z->n_obj + 1);
    w->obj_m = malloc(sizeof(float) * 12 * (w->z->n_obj + 1));
    for (u32 i = 0; i < w->z->n_obj; i++) {
        w->obj_on[i] = 1;
        memcpy(w->obj_m[i], w->obj[i].rot, 9 * sizeof(float));
        memcpy(w->obj_m[i] + 9, w->obj[i].pos, 3 * sizeof(float));
    }
    if (ec) {
        w->pcol = pak + ec->off;
        w->pcol_size = ec->size;
        const PcolHdrW *ph = (const PcolHdrW *)w->pcol;
        if (memcmp(ph->magic, "PCOL", 4) == 0) {
            const PcolPartW *pp = (const PcolPartW *)(w->pcol + ph->off_part);
            w->n_part = ph->n_part;
            w->part_col = malloc(sizeof(s16) * (ph->n_part + 1));
            for (u32 i = 0; i < ph->n_part; i++) {
                char nm[17];
                memcpy(nm, pp[i].name, 16);
                nm[16] = 0;
                w->part_col[i] = (s16)world_find_col(w, nm);
            }
        }
    }
    w->cur_col = -1;
    for (int k = 0; k < 4; k++) w->disp[k] = 0xffffffffu;
    w->far_dist = 50.0f;
    w->lod_dist = 8.0f;    /* eye to the chunk BOX: far LOD never closer than 8 m; ~same tri count as the old 16 m to the
                            * chunk centre, which let far LOD in at ~6 m (spikes on the cliff path) [проверено world_stats] */
    w->decor_dist = 24.0f;
    w->fog_near = 12.0f;
    w->fog_far = 48.0f;
    w->fog_color = 0xff3a3530;   /* ABGR: grey-blue haze [придумал] */
    sceKernelDcacheWritebackAll();
    return 0;
}

int world_upload_vram(World *w, void *vram, int bytes)
{
    int need = 0;
    for (u32 i = 0; i < w->w->n_tex; i++) {
        const WorldTex *t = &w->tex[i];
        int pix = t->psm == GU_PSM_T4 ? t->w * t->h / 2 : t->w * t->h;
        int clut = t->psm == GU_PSM_T4 ? 64 : 1024;
        need += ((pix + 15) & ~15) + ((clut + 15) & ~15);
    }
    if (need > bytes) return 0;
    u8 *dst = (u8 *)vram;
    for (u32 i = 0; i < w->w->n_tex; i++) {
        const WorldTex *t = &w->tex[i];
        int pix = t->psm == GU_PSM_T4 ? t->w * t->h / 2 : t->w * t->h;
        int clut = t->psm == GU_PSM_T4 ? 64 : 1024;
        memcpy(dst, w->tpix[i], pix);
        w->tpix[i] = dst;
        dst += (pix + 15) & ~15;
        memcpy(dst, w->tclut[i], clut);
        w->tclut[i] = dst;
        dst += (clut + 15) & ~15;
    }
    sceKernelDcacheWritebackAll();
    return need;
}

int world_find_col(const World *w, const char *name)
{
    for (u32 i = 0; i < w->z->n_col; i++)
        if (strncmp(w->col[i].name, name, 16) == 0) return (int)i;
    return -1;
}

void world_set_cell(World *w, int zone_col)
{
    w->cur_col = zone_col;
    for (int k = 0; k < 4; k++)
        w->disp[k] = zone_col < 0 ? 0xffffffffu : w->col[zone_col].disp[k];
}

void world_set_cell_by_part(World *w, int part)
{
    if (part < 0 || part >= w->n_part || !w->part_col) return;
    int c = w->part_col[part];
    if (c < 0) return;
    /* collisions with no display groups (h0070B1, h0090B1...) keep the previous cell */
    const u32 *d = w->col[c].disp;
    if (!(d[0] | d[1] | d[2] | d[3])) return;
    if (c != w->cur_col) world_set_cell(w, c);
}

/* ------------------------------------------------------------------ drawing */

static float g_planes[6][4];

static void mat_mul(ScePspFMatrix4 *out, const ScePspFMatrix4 *a, const ScePspFMatrix4 *b)
{
    const float *A = (const float *)a, *B = (const float *)b;
    float *O = (float *)out;
    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 4; r++) {
            float s = 0;
            for (int k = 0; k < 4; k++) s += A[k * 4 + r] * B[c * 4 + k];
            O[c * 4 + r] = s;
        }
}

static void extract_planes(const ScePspFMatrix4 *m)
{
    float r[4][4];
    const float *a = (const float *)m;
    for (int row = 0; row < 4; row++)
        for (int col = 0; col < 4; col++) r[row][col] = a[col * 4 + row];
    for (int i = 0; i < 3; i++)
        for (int sgn = 0; sgn < 2; sgn++) {
            float *p = g_planes[i * 2 + sgn];
            float sign = sgn ? -1.0f : 1.0f;
            for (int k = 0; k < 4; k++) p[k] = r[3][k] + sign * r[i][k];
            float n = sqrtf(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
            if (n > 0) for (int k = 0; k < 4; k++) p[k] /= n;
        }
}

static int sphere_visible(const float c[3], float rad, int planes)
{
    for (int i = 0; i < planes; i++) {
        const float *p = g_planes[i];
        if (p[0] * c[0] + p[1] * c[1] + p[2] * c[2] + p[3] < -rad) return 0;
    }
    return 1;
}

typedef struct { int tex, alpha, cull; } DrawState;

static void set_tex(World *w, DrawState *ds, int ti, WorldStats *st)
{
    if (ti == ds->tex) return;
    const WorldTex *t = &w->tex[ti];
    if (t->psm == GU_PSM_T4) {
        sceGuClutMode(GU_PSM_8888, 0, 0x0f, 0);
        sceGuClutLoad(16 / 8, w->tclut[ti]);
    } else {
        sceGuClutMode(GU_PSM_8888, 0, 0xff, 0);
        sceGuClutLoad(256 / 8, w->tclut[ti]);
    }
    sceGuTexMode(t->psm, 0, 0, 1);
    sceGuTexImage(0, t->w, t->h, t->w, w->tpix[ti]);
    sceGuTexFlush();
    ds->tex = ti;
    if (st) st->tex_changes++;
}

static void set_flags(DrawState *ds, int flags)
{
    int alpha = (flags & WB_ALPHA) ? 1 : 0;
    if (alpha != ds->alpha) {
        if (alpha) sceGuEnable(GU_ALPHA_TEST); else sceGuDisable(GU_ALPHA_TEST);
        ds->alpha = alpha;
    }
    int cull = (flags & WB_NOCULL) ? 0 : 1;
    if (cull != ds->cull) {
        if (cull) sceGuEnable(GU_CULL_FACE); else sceGuDisable(GU_CULL_FACE);
        ds->cull = cull;
    }
}

/* model = [R | t] * T(off) * S(scale); R row-major 3x3 or NULL (identity) */
static void draw_batch(World *w, DrawState *ds, const WorldBatch *b, const float *R, const float *t, WorldStats *st)
{
    ScePspFMatrix4 m;
    const float *s = b->pos_scale, *o = b->pos_off;
    if (R) {
        m.x.x = R[0] * s[0]; m.x.y = R[3] * s[0]; m.x.z = R[6] * s[0]; m.x.w = 0;
        m.y.x = R[1] * s[1]; m.y.y = R[4] * s[1]; m.y.z = R[7] * s[1]; m.y.w = 0;
        m.z.x = R[2] * s[2]; m.z.y = R[5] * s[2]; m.z.z = R[8] * s[2]; m.z.w = 0;
        m.w.x = R[0] * o[0] + R[1] * o[1] + R[2] * o[2] + t[0];
        m.w.y = R[3] * o[0] + R[4] * o[1] + R[5] * o[2] + t[1];
        m.w.z = R[6] * o[0] + R[7] * o[1] + R[8] * o[2] + t[2];
        m.w.w = 1;
    } else {
        memset(&m, 0, sizeof(m));
        m.x.x = s[0]; m.y.y = s[1]; m.z.z = s[2];
        m.w.x = o[0]; m.w.y = o[1]; m.w.z = o[2]; m.w.w = 1;
    }
    sceGuSetMatrix(GU_MODEL, &m);
    set_tex(w, ds, b->tex, st);
    set_flags(ds, b->flags);
    sceGuTexScale(b->uv_scale[0], b->uv_scale[1]);
    sceGuTexOffset(b->uv_off[0], b->uv_off[1]);
    sceGuDrawArray(GU_TRIANGLES, VTYPE, b->idx_count, w->idx + b->idx_start, w->vert + b->vert_start);
    if (st) { st->tris += b->idx_count / 3; st->batches++; }
}

static inline int mask_hit(const u32 *a, const u32 *b)
{
    return ((a[0] & b[0]) | (a[1] & b[1]) | (a[2] & b[2]) | (a[3] & b[3])) != 0;
}

int world_draw(World *w, const ScePspFMatrix4 *proj, const ScePspFMatrix4 *view, const float eye[3], WorldStats *st)
{
    if (st) memset(st, 0, sizeof(*st));
    DrawState ds = {-1, -1, -1};
    ScePspFMatrix4 vp, sky = *proj;
    mat_mul(&vp, proj, view);
    extract_planes(&vp);
    sceGuSetMatrix(GU_VIEW, view);
    sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
    sceGuTexFilter(GU_LINEAR, GU_LINEAR);
    sceGuTexWrap(GU_REPEAT, GU_REPEAT);
    sceGuAlphaFunc(GU_GREATER, 0x80, 0xff);
    sceGuDisable(GU_LIGHTING);
    sceGuShadeModel(GU_SMOOTH);
    const int nb = (int)w->w->n_batch;

    /* 1. sky / backdrop: own far plane, no depth, no fog */
    {
        const float n = 4.0f, f = 6500.0f;
        sky.z.z = (f + n) / (n - f);
        sky.w.z = 2.0f * f * n / (n - f);
        sceGuSetMatrix(GU_PROJECTION, &sky);
        sceGuDisable(GU_FOG);
        sceGuDisable(GU_DEPTH_TEST);
        sceGuDepthMask(GU_TRUE);
        /* dome and mountains, then the alpha-blended clouds (WB_BLEND: soft texture alpha) over them */
        for (int pass = 0; pass < 2; pass++) {
            if (pass == 1) {
                sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
                sceGuEnable(GU_BLEND);
            }
            for (int i = 0; i < nb; i++) {
                const WorldBatch *b = &w->batch[i];
                if (!(b->flags & WB_SKY) || ((b->flags & WB_BLEND) != 0) != (pass == 1)) continue;
                if (st) st->tested++;
                if (!sphere_visible(b->center, b->radius, 4)) continue;
                draw_batch(w, &ds, b, NULL, NULL, st);
            }
        }
        sceGuDisable(GU_BLEND);
        sceGuEnable(GU_DEPTH_TEST);
        sceGuDepthMask(GU_FALSE);
        sceGuSetMatrix(GU_PROJECTION, proj);
    }
    /* 2. far terrain (no fog, no distance cull), then the world in fog */
    for (int pass = 0; pass < 2; pass++) {
        if (pass == 1) {
            sceGuFog(w->fog_near, w->fog_far, w->fog_color);
            sceGuEnable(GU_FOG);
        }
        float far2 = w->far_dist;
        for (int i = 0; i < nb; i++) {
            const WorldBatch *b = &w->batch[i];
            if (b->flags & (WB_SKY | WB_MESH)) continue;
            if (((b->flags & WB_FAR) != 0) != (pass == 0)) continue;
            if (st) st->tested++;
            if (pass == 1) {
                if (!mask_hit(b->mask, w->disp)) continue;
                if (b->flags & (WB_LOD_NEAR | WB_LOD_FAR)) {
                    /* distance from the eye to the chunk's grid cell (box), not to its centre */
                    float ch = w->w->chunk, hc = 0.5f * ch;
                    float cx = fabsf(((int)(b->chunk & 1023) - 512 + 0.5f) * ch - eye[0]) - hc;
                    float cy = fabsf(((int)((b->chunk >> 10) & 1023) - 512 + 0.5f) * ch - eye[1]) - hc;
                    float cz = fabsf(((int)((b->chunk >> 20) & 1023) - 512 + 0.5f) * ch - eye[2]) - hc;
                    cx = cx > 0.0f ? cx : 0.0f;
                    cy = cy > 0.0f ? cy : 0.0f;
                    cz = cz > 0.0f ? cz : 0.0f;
                    int near = cx * cx + cy * cy + cz * cz < w->lod_dist * w->lod_dist;
                    if (near != ((b->flags & WB_LOD_NEAR) != 0)) continue;
                }
                float dx = b->center[0] - eye[0], dy = b->center[1] - eye[1], dz = b->center[2] - eye[2];
                float lim = ((b->flags & WB_DECOR) ? w->decor_dist : far2) + b->radius;
                if (dx * dx + dy * dy + dz * dz > lim * lim) continue;
                /* a small prop the eye is in or next to (no collision for the camera cast: rubble, coffins, column
                   bits) would fill the frame with one dark texture: hide it, as DS1 fades what touches the camera
                   [придумал: radius + 0.6 m; props from 0.5 m
                   (columns) also stop the camera, world_decor_cast; bug "camera in a column", 14.10] */
                if ((b->flags & WB_DECOR) && b->radius < 1.2f) {
                    float nr = b->radius + 0.6f;
                    if (dx * dx + dy * dy + dz * dz < nr * nr) continue;
                }
            }
            if (!sphere_visible(b->center, b->radius, 6)) continue;
            draw_batch(w, &ds, b, NULL, NULL, st);
        }
    }
    /* 3. objects with a mesh (fog walls last: additive, no depth write) */
    for (int pass = 0; pass < 2; pass++) {
        if (pass == 1) {
            sceGuEnable(GU_BLEND);
            sceGuBlendFunc(GU_ADD, GU_FIX, GU_FIX, 0xff707070, 0xffffffff);
            sceGuDepthMask(GU_TRUE);
            sceGuDisable(GU_FOG);
        }
        for (u32 i = 0; i < w->z->n_obj; i++) {
            const ZoneObj *o = &w->obj[i];
            if (o->mesh < 0 || !w->obj_on[i]) continue;
            if ((o->kind == WK_FOG) != (pass == 1)) continue;
            if (!mask_hit(o->mask, w->disp)) continue;
            const WorldMesh *m = &w->mesh[o->mesh];
            const float *M = w->obj_m[i];
            if (!sphere_visible(M + 9, m->radius, 6)) continue;
            for (u32 k = 0; k < m->batch_count; k++)
                draw_batch(w, &ds, &w->batch[m->batch_start + k], M, M + 9, st);
            if (st) st->objs++;
        }
    }
    sceGuDisable(GU_BLEND);
    sceGuDepthMask(GU_FALSE);
    sceGuEnable(GU_FOG);
    sceGuDisable(GU_ALPHA_TEST);
    sceGuEnable(GU_CULL_FACE);
    sceGuTexScale(1.0f, 1.0f);
    sceGuTexOffset(0.0f, 0.0f);
    {
        ScePspFMatrix4 id;
        memset(&id, 0, sizeof(id));
        id.x.x = id.y.y = id.z.z = id.w.w = 1.0f;
        sceGuSetMatrix(GU_MODEL, &id);
    }
    return st ? st->tris : 0;
}

/* ------------------------------------------------------------------ entities */

int world_find_region(const World *w, int role, int nth)
{
    for (u32 i = 0; i < w->z->n_reg; i++)
        if (w->reg[i].role == role && nth-- == 0) return (int)i;
    return -1;
}

int world_find_obj(const World *w, u32 entity)
{
    for (u32 i = 0; i < w->z->n_obj; i++)
        if (w->obj[i].entity == entity) return (int)i;
    return -1;
}

int world_find_chr(const World *w, u32 entity)
{
    for (u32 i = 0; i < w->z->n_chr; i++)
        if (w->chr[i].entity == entity) return (int)i;
    return -1;
}

void world_obj_enable(World *w, int obj, int on)
{
    if (obj >= 0 && obj < (int)w->z->n_obj) w->obj_on[obj] = on ? 1 : 0;
}

int world_in_region(const World *w, int ri, const float p[3])
{
    const ZoneReg *r = &w->reg[ri];
    float dx = p[0] - r->pos[0], dy = p[1] - r->pos[1], dz = p[2] - r->pos[2];
    switch (r->shape) {
    case WS_BOX: {
        float c = cosf(r->yaw), s = sinf(r->yaw);
        float lx = c * dx - s * dz, lz = s * dx + c * dz;
        return fabsf(lx) <= r->size[0] * 0.5f && fabsf(lz) <= r->size[2] * 0.5f && dy >= 0 && dy <= r->size[1];
    }
    case WS_SPHERE: return dx * dx + dy * dy + dz * dz <= r->size[0] * r->size[0];
    case WS_CYLINDER: return dx * dx + dz * dz <= r->size[0] * r->size[0] && dy >= 0 && dy <= r->size[1];
    default: return dx * dx + dy * dy + dz * dz <= 1.0f;
    }
}

/* world -> object local: l = R^T (p - t) */
static void to_local(const float *M, const float p[3], float l[3])
{
    float d0 = p[0] - M[9], d1 = p[1] - M[10], d2 = p[2] - M[11];
    l[0] = M[0] * d0 + M[3] * d1 + M[6] * d2;
    l[1] = M[1] * d0 + M[4] * d1 + M[7] * d2;
    l[2] = M[2] * d0 + M[5] * d1 + M[8] * d2;
}

int world_obj_push(World *w, float pos[3], float radius, float height)
{
    return world_obj_push_from(w, pos, 0, radius, height);
}

/* Thin boxes (door leaves are 0.2 m thick, the knight's radius is 0.35 m): a fast step (the Demon's knockback, a
   body push) can carry the centre into or through the box, and the shallow-axis rule then throws it out on the
   FAR side - behind a closed door. With prev (the position before this step) the side is kept: if the centre
   crossed a face it was outside of, it goes back out through that face. */
int world_obj_push_from(World *w, float pos[3], const float prev[3], float radius, float height)
{
    int hits = 0;
    for (u32 i = 0; i < w->z->n_obj; i++) {
        const ZoneObj *o = &w->obj[i];
        if (!w->obj_on[i] || !o->box_count) continue;
        const float *M = w->obj_m[i];
        float dx0 = pos[0] - M[9], dz0 = pos[2] - M[11];
        if (dx0 * dx0 + dz0 * dz0 > 15.0f * 15.0f) continue;
        for (int k = 0; k < o->box_count; k++) {
            const ZoneBox *b = &w->box[o->box_start + k];
            float l[3];
            to_local(M, pos, l);
            /* vertical overlap (object Y ~ world Y: all m18_01 doors rotate about Y only) */
            if (l[1] + height < b->c[1] - b->h[1] || l[1] > b->c[1] + b->h[1]) continue;
            /* a box whose top is within a step of the feet is a floor to walk onto (the pit cover o8510 in the hall,
               world_obj_floor), not a wall */
            if (b->c[1] + b->h[1] <= l[1] + 0.45f) continue;
            float qx = l[0] - b->c[0], qz = l[2] - b->c[2];
            if (prev) {
                float lp[3], q[2] = {qx, qz}, qp[2], h[2] = {b->h[0], b->h[2]}, best = -1.0f;
                int ax = -1;
                to_local(M, prev, lp);
                qp[0] = lp[0] - b->c[0]; qp[1] = lp[2] - b->c[2];
                for (int a = 0; a < 2; a++) {
                    float sg = qp[a] < 0 ? -1.0f : 1.0f;
                    /* prev centre outside this face, new centre at or past it */
                    if (fabsf(qp[a]) < h[a] || q[a] * sg >= h[a]) continue;
                    float t = (sg * h[a] - qp[a]) / (q[a] - qp[a]), o = qp[1 - a] + t * (q[1 - a] - qp[1 - a]);
                    if (fabsf(o) <= h[1 - a] && t > best) { best = t; ax = a; }
                }
                if (ax >= 0) {
                    float sg = qp[ax] < 0 ? -1.0f : 1.0f, px = 0, pz = 0, dd = sg * (h[ax] + radius) - q[ax];
                    if (ax == 0) px = dd; else pz = dd;
                    pos[0] += M[0] * px + M[2] * pz;
                    pos[1] += M[3] * px + M[5] * pz;
                    pos[2] += M[6] * px + M[8] * pz;
                    hits++;
                    continue;
                }
            }
            float cx = qx < -b->h[0] ? -b->h[0] : (qx > b->h[0] ? b->h[0] : qx);
            float cz = qz < -b->h[2] ? -b->h[2] : (qz > b->h[2] ? b->h[2] : qz);
            float ex = qx - cx, ez = qz - cz, d2 = ex * ex + ez * ez;
            float px, pz;
            if (d2 > radius * radius) continue;
            if (d2 > 1e-8f) {
                float d = sqrtf(d2), k2 = (radius - d) / d;
                px = ex * k2; pz = ez * k2;
            } else {   /* centre inside the box: out along the shallow axis */
                float ox = b->h[0] - fabsf(qx) + radius, oz = b->h[2] - fabsf(qz) + radius;
                if (ox < oz) { px = qx < 0 ? -ox : ox; pz = 0; } else { px = 0; pz = qz < 0 ? -oz : oz; }
            }
            /* local delta (px, 0, pz) -> world: R * d */
            pos[0] += M[0] * px + M[2] * pz;
            pos[1] += M[3] * px + M[5] * pz;
            pos[2] += M[6] * px + M[8] * pz;
            hits++;
        }
    }
    return hits;
}

float world_obj_cast(const World *w, const float a[3], const float b[3], float r)
{
    float best = 1.0f;
    float mx = 0.5f * (a[0] + b[0]), mz = 0.5f * (a[2] + b[2]);
    float hl = 0.5f * sqrtf((b[0] - a[0]) * (b[0] - a[0]) + (b[2] - a[2]) * (b[2] - a[2]));
    for (u32 i = 0; i < w->z->n_obj; i++) {
        const ZoneObj *o = &w->obj[i];
        if (!w->obj_on[i] || !o->box_count || o->kind == WK_FOG) continue;
        const float *M = w->obj_m[i];
        float dx0 = mx - M[9], dz0 = mz - M[11], reach = hl + 15.0f;
        if (dx0 * dx0 + dz0 * dz0 > reach * reach) continue;
        float la[3], lb[3];
        to_local(M, a, la);
        to_local(M, b, lb);
        float d[3] = {lb[0] - la[0], lb[1] - la[1], lb[2] - la[2]};
        for (int k = 0; k < o->box_count; k++) {
            const ZoneBox *bx = &w->box[o->box_start + k];
            /* slab test of the segment against the box grown by r (rounded corners ignored: conservative) */
            float t0 = 0, t1 = 1;
            int inside = 1, miss = 0;
            for (int ax = 0; ax < 3 && !miss; ax++) {
                float lo = bx->c[ax] - bx->h[ax] - r, hi = bx->c[ax] + bx->h[ax] + r;
                if (la[ax] < lo || la[ax] > hi) inside = 0;
                if (fabsf(d[ax]) < 1e-6f) { if (la[ax] < lo || la[ax] > hi) miss = 1; continue; }
                float ta = (lo - la[ax]) / d[ax], tb = (hi - la[ax]) / d[ax];
                if (ta > tb) { float tt = ta; ta = tb; tb = tt; }
                if (ta > t0) t0 = ta;
                if (tb < t1) t1 = tb;
                if (t0 > t1) miss = 1;
            }
            if (miss || inside) continue;   /* the pivot already in a box (hugging a door): ignore it, as pcol does */
            if (t0 < best) best = t0;
        }
    }
    return best;
}

float world_decor_cast(const World *w, const float a[3], const float b[3], float r)
{
    float D[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
    float len2 = D[0] * D[0] + D[1] * D[1] + D[2] * D[2];
    if (len2 < 1e-8f) return 1.0f;
    float len = sqrtf(len2), best = 1.0f;
    for (u32 i = 0; i < w->w->n_batch; i++) {
        const WorldBatch *wb = &w->batch[i];
        if (!(wb->flags & WB_DECOR) || (wb->flags & WB_MESH) || wb->radius < 0.5f) continue;
        if (!mask_hit(wb->mask, w->disp)) continue;
        /* segment vs bounding sphere */
        float o[3] = {wb->center[0] - a[0], wb->center[1] - a[1], wb->center[2] - a[2]};
        float t = (o[0] * D[0] + o[1] * D[1] + o[2] * D[2]) / len2;
        if (t < 0) t = 0; else if (t > 1) t = 1;
        float q[3] = {o[0] - D[0] * t, o[1] - D[1] * t, o[2] - D[2] * t}, rr = wb->radius + r;
        if (q[0] * q[0] + q[1] * q[1] + q[2] * q[2] > rr * rr) continue;
        const float *so = wb->pos_off;
        float sc[3] = {wb->pos_scale[0] / 32768.0f, wb->pos_scale[1] / 32768.0f, wb->pos_scale[2] / 32768.0f};
        for (u32 k = 0; k + 2 < wb->idx_count; k += 3) {
            float v[3][3];
            for (int j = 0; j < 3; j++) {
                const WorldVert *wv = &w->vert[wb->vert_start + w->idx[wb->idx_start + k + j]];
                v[j][0] = so[0] + sc[0] * wv->x; v[j][1] = so[1] + sc[1] * wv->y; v[j][2] = so[2] + sc[2] * wv->z;
            }
            /* Moller-Trumbore, two-sided */
            float e1[3], e2[3], tv[3], pv[3], qv[3];
            for (int j = 0; j < 3; j++) { e1[j] = v[1][j] - v[0][j]; e2[j] = v[2][j] - v[0][j]; tv[j] = a[j] - v[0][j]; }
            pv[0] = D[1] * e2[2] - D[2] * e2[1]; pv[1] = D[2] * e2[0] - D[0] * e2[2]; pv[2] = D[0] * e2[1] - D[1] * e2[0];
            float det = e1[0] * pv[0] + e1[1] * pv[1] + e1[2] * pv[2];
            if (fabsf(det) < 1e-9f) continue;
            float inv = 1.0f / det, u = (tv[0] * pv[0] + tv[1] * pv[1] + tv[2] * pv[2]) * inv;
            if (u < 0 || u > 1) continue;
            qv[0] = tv[1] * e1[2] - tv[2] * e1[1]; qv[1] = tv[2] * e1[0] - tv[0] * e1[2]; qv[2] = tv[0] * e1[1] - tv[1] * e1[0];
            float vv = (D[0] * qv[0] + D[1] * qv[1] + D[2] * qv[2]) * inv;
            if (vv < 0 || u + vv > 1) continue;
            float th = (e2[0] * qv[0] + e2[1] * qv[1] + e2[2] * qv[2]) * inv;
            if (th < 0 || th > 1 || th * len < r) continue;   /* behind, beyond, or what the pivot already touches */
            th -= r / len;                                      /* keep the camera sphere in front of the face */
            if (th < best) best = th;
        }
    }
    return best < 0 ? 0 : best;
}

int world_obj_floor(const World *w, float x, float z, float y_top, float y_bottom, float *h)
{
    int found = 0;
    float best = -1e30f;
    float p[3] = {x, 0, z};
    for (u32 i = 0; i < w->z->n_obj; i++) {
        const ZoneObj *o = &w->obj[i];
        if (!w->obj_on[i] || !o->box_count || o->kind == WK_FOG) continue;
        const float *M = w->obj_m[i];
        float l[3];
        p[1] = M[10];
        to_local(M, p, l);
        for (int k = 0; k < o->box_count; k++) {
            const ZoneBox *b = &w->box[o->box_start + k];
            if (fabsf(l[0] - b->c[0]) > b->h[0] || fabsf(l[2] - b->c[2]) > b->h[2]) continue;
            float top = M[10] + b->c[1] + b->h[1];
            if (top <= y_top && top >= y_bottom && top > best) { best = top; found = 1; }
        }
    }
    if (found && h) *h = best;
    return found;
}
