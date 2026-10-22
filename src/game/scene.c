#include "scene.h"

#include <malloc.h>
#include <math.h>
#include <pspgum.h>
#include <pspiofilemgr.h>
#include <pspkernel.h>
#include <stdio.h>
#include <string.h>

#define BF_ALPHA 1
#define BF_NOCULL 2

static const char *g_prefixes[] = {"", "umd0:/", "host0:/", "ms0:/ash/", "disc0:/", NULL};

int ash_load_file(const char *name, u8 **out, char *found, int found_len)
{
    char path[256];
    for (int i = 0; g_prefixes[i]; i++) {
        snprintf(path, sizeof(path), "%s%s", g_prefixes[i], name);
        SceUID fd = sceIoOpen(path, PSP_O_RDONLY, 0);
        if (fd < 0) continue;
        int size = (int)sceIoLseek(fd, 0, PSP_SEEK_END);
        sceIoLseek(fd, 0, PSP_SEEK_SET);
        u8 *buf = memalign(64, size + 16);
        if (!buf) { sceIoClose(fd); printf("ash: out of memory for %s (%d B)\n", path, size); return -1; }
        int got = 0;
        while (got < size) {
            int r = sceIoRead(fd, buf + got, size - got);
            if (r <= 0) break;
            got += r;
        }
        sceIoClose(fd);
        buf[got] = 0;
        if (found) snprintf(found, found_len, "%s", path);
        *out = buf;
        return got;
    }
    return -1;
}

void ash_mat_mul(ScePspFMatrix4 *out, const ScePspFMatrix4 *a, const ScePspFMatrix4 *b)
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

int scene_load(Scene *s, u8 *file, int size)
{
    if (size < (int)sizeof(AshmHeader)) return -1;
    s->file = file;
    s->hdr = (AshmHeader *)file;
    if (memcmp(s->hdr->magic, "ASHM", 4) != 0 || s->hdr->version != 1) return -2;
    s->tex = (AshmTex *)(file + s->hdr->off_tex);
    s->batch = (AshmBatch *)(file + s->hdr->off_batch);
    s->vert = (AshmVert *)(file + s->hdr->off_vert);
    s->idx = (u16 *)(file + s->hdr->off_idx);
    s->cam = (AshmCam *)(file + s->hdr->off_cam);
    sceKernelDcacheWritebackAll();
    return 0;
}

static float g_planes[6][4];

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

static int sphere_visible(float x, float y, float z, float rad)
{
    for (int i = 0; i < 6; i++) {
        const float *p = g_planes[i];
        if (p[0] * x + p[1] * y + p[2] * z + p[3] < -rad) return 0;
    }
    return 1;
}

int scene_draw(const Scene *s, const ScePspFMatrix4 *vp, int *batches)
{
    extract_planes(vp);
    int cur_tex = -1, cur_alpha = -1, cur_cull = -1, tris = 0, n = 0;
    for (unsigned int i = 0; i < s->hdr->n_batch; i++) {
        const AshmBatch *b = &s->batch[i];
        if (!sphere_visible(b->cx, b->cy, b->cz, b->radius)) continue;
        if (b->tex != cur_tex) {
            const AshmTex *t = &s->tex[b->tex];
            sceGuClutMode(GU_PSM_8888, 0, 0xff, 0);
            sceGuClutLoad(256 / 8, s->file + t->off_clut);
            sceGuTexMode(GU_PSM_T8, 0, 0, 1);
            sceGuTexImage(0, t->w, t->h, t->w, s->file + t->off_pix);
            sceGuTexFlush();
            cur_tex = b->tex;
        }
        int alpha = (b->flags & BF_ALPHA) ? 1 : 0;
        if (alpha != cur_alpha) {
            if (alpha) sceGuEnable(GU_ALPHA_TEST); else sceGuDisable(GU_ALPHA_TEST);
            cur_alpha = alpha;
        }
        int cull = (b->flags & BF_NOCULL) ? 0 : 1;
        if (cull != cur_cull) {
            if (cull) sceGuEnable(GU_CULL_FACE); else sceGuDisable(GU_CULL_FACE);
            cur_cull = cull;
        }
        sceGumDrawArray(GU_TRIANGLES,
                        GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_INDEX_16BIT | GU_TRANSFORM_3D,
                        b->idx_count, s->idx + b->idx_start, s->vert + b->vert_start);
        tris += b->idx_count / 3;
        n++;
    }
    sceGuDisable(GU_ALPHA_TEST);
    sceGuEnable(GU_CULL_FACE);
    if (batches) *batches = n;
    return tris;
}
