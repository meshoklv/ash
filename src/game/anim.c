#include "anim.h"

#include <math.h>
#include <string.h>

static int g_use_vfpu = 1;

void anim_set_vfpu(int on) { g_use_vfpu = on; }
int anim_vfpu(void) { return g_use_vfpu; }

int anim_load(AnimSet *set, u8 *file, int size)
{
    if (size < (int)sizeof(AnimFileHdr)) return -1;
    AnimFileHdr *h = (AnimFileHdr *)file;
    if (memcmp(h->magic, "ASHA", 4) != 0 || h->version != 1 || h->n_bones > ANIM_MAX_BONES) return -2;
    set->file = file;
    set->hdr = h;
    set->clips = (AnimClip *)(file + h->off_clips);
    set->n_bones = (int)h->n_bones;
    return 0;
}

int anim_find(const AnimSet *set, const char *name)
{
    for (u32 i = 0; i < set->hdr->n_clips; i++)
        if (strncmp(set->clips[i].name, name, 16) == 0) return (int)i;
    return -1;
}

float anim_duration(const AnimSet *set, int clip)
{
    return (float)(set->clips[clip].n_frames - 1) / set->hdr->fps;
}

/* frame position for time t: f0, f1, fraction */
static void frame_of(const AnimSet *set, const AnimClip *c, float t, int *f0, int *f1, float *a)
{
    int last = c->n_frames - 1;
    float f = t * set->hdr->fps;
    if (c->flags & CLIP_LOOP) {
        if (last <= 0) { *f0 = *f1 = 0; *a = 0; return; }
        f = fmodf(f, (float)last);
        if (f < 0) f += (float)last;
    } else {
        if (f < 0) f = 0;
        if (f > (float)last) f = (float)last;
    }
    int i = (int)f;
    if (i >= last) { *f0 = *f1 = last; *a = 0; return; }
    *f0 = i;
    *f1 = i + 1;
    *a = f - (float)i;
}

void anim_sample(const AnimSet *set, int clip, float t, BonePose *out)
{
    const AnimClip *c = &set->clips[clip];
    const int nb = set->n_bones;
    int f0, f1;
    float a;
    frame_of(set, c, t, &f0, &f1, &a);
    const s16 *r0 = (const s16 *)(set->file + c->off_rot) + f0 * nb * 4;
    const s16 *r1 = (const s16 *)(set->file + c->off_rot) + f1 * nb * 4;
    const u16 *ts = (const u16 *)(set->file + c->off_tstart);
    const u8 *tf = set->file + c->off_tflag;
    const s16 *tr = (const s16 *)(set->file + c->off_trans);
    const float qs = 1.0f / 32767.0f, ts_inv = 1.0f / set->hdr->t_scale;
    const float b = 1.0f - a;
    for (int k = 0; k < nb; k++) {
        /* keys are hemisphere-continuous (cooker), so plain nlerp */
        float x = (r0[0] * b + r1[0] * a) * qs, y = (r0[1] * b + r1[1] * a) * qs;
        float z = (r0[2] * b + r1[2] * a) * qs, w = (r0[3] * b + r1[3] * a) * qs;
        float n = x * x + y * y + z * z + w * w;
        n = n > 1e-12f ? 1.0f / sqrtf(n) : 0.0f;
        BonePose *o = &out[k];
        o->q[0] = x * n; o->q[1] = y * n; o->q[2] = z * n; o->q[3] = w * n;
        if (tf[k]) {
            const s16 *p0 = tr + (ts[k] + f0) * 3, *p1 = tr + (ts[k] + f1) * 3;
            o->t[0] = (p0[0] * b + p1[0] * a) * ts_inv;
            o->t[1] = (p0[1] * b + p1[1] * a) * ts_inv;
            o->t[2] = (p0[2] * b + p1[2] * a) * ts_inv;
        } else {
            const s16 *p = tr + ts[k] * 3;
            o->t[0] = p[0] * ts_inv; o->t[1] = p[1] * ts_inv; o->t[2] = p[2] * ts_inv;
        }
        r0 += 4; r1 += 4;
    }
}

void anim_blend(int n_bones, const BonePose *a, const BonePose *b, float w, BonePose *out)
{
    const float v = 1.0f - w;
    for (int k = 0; k < n_bones; k++) {
        const float *qa = a[k].q, *qb = b[k].q;
        float s = (qa[0] * qb[0] + qa[1] * qb[1] + qa[2] * qb[2] + qa[3] * qb[3]) < 0 ? -w : w;
        float x = qa[0] * v + qb[0] * s, y = qa[1] * v + qb[1] * s, z = qa[2] * v + qb[2] * s, q = qa[3] * v + qb[3] * s;
        float n = x * x + y * y + z * z + q * q;
        n = n > 1e-12f ? 1.0f / sqrtf(n) : 0.0f;
        BonePose *o = &out[k];
        float t0 = a[k].t[0] * v + b[k].t[0] * w, t1 = a[k].t[1] * v + b[k].t[1] * w, t2 = a[k].t[2] * v + b[k].t[2] * w;
        o->q[0] = x * n; o->q[1] = y * n; o->q[2] = z * n; o->q[3] = q * n;
        o->t[0] = t0; o->t[1] = t1; o->t[2] = t2;
    }
}

static void root_at(const AnimSet *set, const AnimClip *c, float f, float out[4])
{
    const float *r = (const float *)(set->file + c->off_root);
    int last = c->n_frames - 1;
    if (f <= 0) { for (int i = 0; i < 4; i++) out[i] = r[i]; return; }
    if (f >= (float)last) { for (int i = 0; i < 4; i++) out[i] = r[last * 4 + i]; return; }
    int i0 = (int)f;
    float a = f - (float)i0;
    for (int i = 0; i < 4; i++) out[i] = r[i0 * 4 + i] * (1.0f - a) + r[(i0 + 1) * 4 + i] * a;
}

void anim_root_delta(const AnimSet *set, int clip, float t0, float t1, float out[4])
{
    const AnimClip *c = &set->clips[clip];
    const float fps = set->hdr->fps;
    const int last = c->n_frames - 1;
    float a[4], b[4];
    out[0] = out[1] = out[2] = out[3] = 0;
    if (last <= 0 || t1 <= t0) return;
    if (!(c->flags & CLIP_LOOP)) {
        root_at(set, c, t0 * fps, a);
        root_at(set, c, t1 * fps, b);
        for (int i = 0; i < 4; i++) out[i] = b[i] - a[i];
        return;
    }
    float f0 = t0 * fps, f1 = t1 * fps;
    float cycles0 = floorf(f0 / (float)last), cycles1 = floorf(f1 / (float)last);
    float full[4];
    root_at(set, c, (float)last, full);
    root_at(set, c, f0 - cycles0 * (float)last, a);
    root_at(set, c, f1 - cycles1 * (float)last, b);
    float n = cycles1 - cycles0;
    for (int i = 0; i < 4; i++) out[i] = b[i] - a[i] + n * full[i];
}

/* ------------------------------------------------------------------ matrices (column-major, like pspgum) */

static void quat_to_mat(const BonePose *p, ScePspFMatrix4 *m)
{
    float x = p->q[0], y = p->q[1], z = p->q[2], w = p->q[3];
    float xx = x * x, yy = y * y, zz = z * z, xy = x * y, xz = x * z, yz = y * z, wx = w * x, wy = w * y, wz = w * z;
    m->x.x = 1 - 2 * (yy + zz); m->x.y = 2 * (xy + wz);     m->x.z = 2 * (xz - wy);     m->x.w = 0;
    m->y.x = 2 * (xy - wz);     m->y.y = 1 - 2 * (xx + zz); m->y.z = 2 * (yz + wx);     m->y.w = 0;
    m->z.x = 2 * (xz + wy);     m->z.y = 2 * (yz - wx);     m->z.z = 1 - 2 * (xx + yy); m->z.w = 0;
    m->w.x = p->t[0];           m->w.y = p->t[1];           m->w.z = p->t[2];           m->w.w = 1;
}

/* out = a * b for affine matrices (w row = 0 0 0 1); out must not alias a or b. */
static void mul_affine_c(ScePspFMatrix4 *out, const ScePspFMatrix4 *a, const ScePspFMatrix4 *b)
{
    const float *A = (const float *)a, *B = (const float *)b;
    float *O = (float *)out;
    for (int c = 0; c < 4; c++) {
        const float b0 = B[c * 4 + 0], b1 = B[c * 4 + 1], b2 = B[c * 4 + 2], b3 = B[c * 4 + 3];
        O[c * 4 + 0] = A[0] * b0 + A[4] * b1 + A[8] * b2 + A[12] * b3;
        O[c * 4 + 1] = A[1] * b0 + A[5] * b1 + A[9] * b2 + A[13] * b3;
        O[c * 4 + 2] = A[2] * b0 + A[6] * b1 + A[10] * b2 + A[14] * b3;
        O[c * 4 + 3] = b3;
    }
}

/* VFPU: out = a * b, full 4x4, all 16-byte aligned. */
static void mul_vfpu(ScePspFMatrix4 *out, const ScePspFMatrix4 *a, const ScePspFMatrix4 *b)
{
#ifndef __psp__   /* native build (tests/native): no VFPU, plain C */
    mul_affine_c(out, a, b);
#else
    __asm__ volatile(
        "lv.q C000, 0(%1)\n"
        "lv.q C010, 16(%1)\n"
        "lv.q C020, 32(%1)\n"
        "lv.q C030, 48(%1)\n"
        "lv.q C100, 0(%2)\n"
        "lv.q C110, 16(%2)\n"
        "lv.q C120, 32(%2)\n"
        "lv.q C130, 48(%2)\n"
        "vmmul.q M200, M000, M100\n"
        "sv.q C200, 0(%0)\n"
        "sv.q C210, 16(%0)\n"
        "sv.q C220, 32(%0)\n"
        "sv.q C230, 48(%0)\n"
        :
        : "r"(out), "r"(a), "r"(b)
        : "memory");
#endif
}

static void mul(ScePspFMatrix4 *out, const ScePspFMatrix4 *a, const ScePspFMatrix4 *b)
{
    if (g_use_vfpu) mul_vfpu(out, a, b); else mul_affine_c(out, a, b);
}

void anim_skin_matrices(int n_bones, const int *parent, const BonePose *local, const ScePspFMatrix4 *inv_bind,
                        ScePspFMatrix4 *world, ScePspFMatrix4 *skin)
{
    ScePspFMatrix4 __attribute__((aligned(16))) l;
    for (int k = 0; k < n_bones; k++) {
        int p = parent[k];
        if (p < 0) {
            quat_to_mat(&local[k], &world[k]);
        } else {
            quat_to_mat(&local[k], &l);
            mul(&world[k], &world[p], &l);
        }
        mul(&skin[k], &world[k], &inv_bind[k]);
    }
}

float anim_selftest_vfpu(void)
{
    ScePspFMatrix4 __attribute__((aligned(16))) a, b, r1, r2;
    BonePose pa = {{0.1f, 0.7f, -0.2f, 0.67f}, {0.3f, -1.2f, 2.5f}, 0};
    BonePose pb = {{-0.5f, 0.1f, 0.4f, 0.75f}, {-0.7f, 0.05f, 0.9f}, 0};
    float n = sqrtf(0.01f + 0.49f + 0.04f + 0.67f * 0.67f);
    for (int i = 0; i < 4; i++) pa.q[i] /= n;
    n = sqrtf(0.25f + 0.01f + 0.16f + 0.75f * 0.75f);
    for (int i = 0; i < 4; i++) pb.q[i] /= n;
    quat_to_mat(&pa, &a);
    quat_to_mat(&pb, &b);
    mul_affine_c(&r1, &a, &b);
    mul_vfpu(&r2, &a, &b);
    float d = 0;
    for (int i = 0; i < 16; i++) {
        float e = fabsf(((float *)&r1)[i] - ((float *)&r2)[i]);
        if (e > d) d = e;
    }
    if (d > 1e-3f) g_use_vfpu = 0;
    return d;
}
