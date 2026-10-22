#include "pcol.h"

#include <malloc.h>
#include <math.h>
#include <string.h>

_Static_assert(sizeof(PcolHdr) == 80, "PcolHdr");
_Static_assert(sizeof(PcolTri) == 10, "PcolTri");
_Static_assert(sizeof(PcolPart) == 24, "PcolPart");

#define MAX_CAND 4096   /* whole-zone pcol: one 2 m cell holds up to 623 triangles; a capsule query spans 4-9 */

static u16 g_cand[MAX_CAND];

int pcol_load(Pcol *c, u8 *file, int size)
{
    memset(c, 0, sizeof(*c));
    if (size < (int)sizeof(PcolHdr) || memcmp(file, "PCOL", 4) != 0) return -1;
    const PcolHdr *h = (const PcolHdr *)file;
    if (h->version != 1) return -2;
    c->file = file;
    c->hdr = h;
    c->vert = (const float *)(file + h->off_vert);
    c->tri = (const PcolTri *)(file + h->off_tri);
    c->part = (const PcolPart *)(file + h->off_part);
    c->cell = (const PcolCell *)(file + h->off_cell);
    c->list = (const u16 *)(file + h->off_list);
    c->plane = memalign(16, 16 * h->n_tri);
    c->stamp = calloc(h->n_tri, sizeof(u16));
    if (!c->plane || !c->stamp) return -3;
    for (u32 i = 0; i < h->n_tri; i++) {
        const float *a = c->vert + 3 * c->tri[i].v[0], *b = c->vert + 3 * c->tri[i].v[1],
                    *d = c->vert + 3 * c->tri[i].v[2];
        float e1[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, e2[3] = {d[0] - a[0], d[1] - a[1], d[2] - a[2]};
        float n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
        float l = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (l < 1e-12f) l = 1;
        float *p = c->plane + 4 * i;
        p[0] = n[0] / l; p[1] = n[1] / l; p[2] = n[2] / l;
        p[3] = p[0] * a[0] + p[1] * a[1] + p[2] * a[2];
    }
    c->cur_stamp = 0;
    return 0;
}

int pcol_part_find(const Pcol *c, const char *name)
{
    for (u32 i = 0; i < c->hdr->n_part; i++)
        if (!strncmp(c->part[i].name, name, sizeof(c->part[i].name))) return (int)i;
    return -1;
}

void pcol_part_enable(Pcol *c, int part, int on)
{
    if (part >= 0 && part < 256) c->part_off[part] = !on;
}

int pcol_cam_row(const Pcol *c, int tri)
{
    if (tri < 0 || (u32)tri >= c->hdr->n_tri) return -1;
    return c->part[c->tri[tri].part].cam_row;
}

/* Unique triangles whose cells overlap the XZ box. */
static int gather(Pcol *c, float x0, float z0, float x1, float z1)
{
    const PcolHdr *h = c->hdr;
    int i0 = (int)floorf((x0 - h->x0) / h->cell), i1 = (int)floorf((x1 - h->x0) / h->cell);
    int j0 = (int)floorf((z0 - h->z0) / h->cell), j1 = (int)floorf((z1 - h->z0) / h->cell);
    if (i0 < 0) i0 = 0;
    if (j0 < 0) j0 = 0;
    if (i1 >= (int)h->nx) i1 = h->nx - 1;
    if (j1 >= (int)h->nz) j1 = h->nz - 1;
    if (++c->cur_stamp == 0) { memset(c->stamp, 0, h->n_tri * sizeof(u16)); c->cur_stamp = 1; }
    u16 s = c->cur_stamp;
    int n = 0;
    for (int j = j0; j <= j1; j++)
        for (int i = i0; i <= i1; i++) {
            const PcolCell *cl = &c->cell[j * h->nx + i];
            const u16 *l = c->list + cl->start;
            for (u32 k = 0; k < cl->count; k++) {
                u16 t = l[k];
                if (c->stamp[t] == s) continue;
                c->stamp[t] = s;
                if (c->part_off[c->tri[t].part]) continue;   /* switched off by an event (pcol_part_enable) */
                if (n < MAX_CAND) g_cand[n++] = t;
                else c->overflow++;
            }
        }
    return n;
}

/* Ericson, Real-Time Collision Detection 5.1.5 */
static void closest_on_tri(const float p[3], const float a[3], const float b[3], const float c[3], float out[3])
{
    float ab[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, ac[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
    float ap[3] = {p[0] - a[0], p[1] - a[1], p[2] - a[2]};
#define DOT(u, v) ((u)[0] * (v)[0] + (u)[1] * (v)[1] + (u)[2] * (v)[2])
    float d1 = DOT(ab, ap), d2 = DOT(ac, ap);
    if (d1 <= 0 && d2 <= 0) { memcpy(out, a, 12); return; }
    float bp[3] = {p[0] - b[0], p[1] - b[1], p[2] - b[2]};
    float d3 = DOT(ab, bp), d4 = DOT(ac, bp);
    if (d3 >= 0 && d4 <= d3) { memcpy(out, b, 12); return; }
    float vc = d1 * d4 - d3 * d2;
    if (vc <= 0 && d1 >= 0 && d3 <= 0) {
        float v = d1 - d3 != 0 ? d1 / (d1 - d3) : 0;
        for (int i = 0; i < 3; i++) out[i] = a[i] + v * ab[i];
        return;
    }
    float cp[3] = {p[0] - c[0], p[1] - c[1], p[2] - c[2]};
    float d5 = DOT(ab, cp), d6 = DOT(ac, cp);
    if (d6 >= 0 && d5 <= d6) { memcpy(out, c, 12); return; }
    float vb = d5 * d2 - d1 * d6;
    if (vb <= 0 && d2 >= 0 && d6 <= 0) {
        float w = d2 - d6 != 0 ? d2 / (d2 - d6) : 0;
        for (int i = 0; i < 3; i++) out[i] = a[i] + w * ac[i];
        return;
    }
    float va = d3 * d6 - d5 * d4;
    if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) {
        float sw = (d4 - d3) + (d5 - d6), w = sw != 0 ? (d4 - d3) / sw : 0;
        for (int i = 0; i < 3; i++) out[i] = b[i] + w * (c[i] - b[i]);
        return;
    }
    float sum = va + vb + vc;
    if (sum == 0) { memcpy(out, a, 12); return; }
    float den = 1.0f / sum, v = vb * den, w = vc * den;
    for (int i = 0; i < 3; i++) out[i] = a[i] + ab[i] * v + ac[i] * w;
}

#define V(c, t, k) ((c)->vert + 3 * (c)->tri[t].v[k])

/* Moller-Trumbore, two-sided. */
static int ray_tri(const Pcol *c, int t, const float o[3], const float d[3], float *tout)
{
    const float *a = V(c, t, 0), *b = V(c, t, 1), *cc = V(c, t, 2);
    float e1[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, e2[3] = {cc[0] - a[0], cc[1] - a[1], cc[2] - a[2]};
    float p[3] = {d[1] * e2[2] - d[2] * e2[1], d[2] * e2[0] - d[0] * e2[2], d[0] * e2[1] - d[1] * e2[0]};
    float det = DOT(e1, p);
    if (fabsf(det) < 1e-12f) return 0;
    float inv = 1.0f / det;
    float s[3] = {o[0] - a[0], o[1] - a[1], o[2] - a[2]};
    float u = DOT(s, p) * inv;
    if (u < 0 || u > 1) return 0;
    float q[3] = {s[1] * e1[2] - s[2] * e1[1], s[2] * e1[0] - s[0] * e1[2], s[0] * e1[1] - s[1] * e1[0]};
    float v = DOT(d, q) * inv;
    if (v < 0 || u + v > 1) return 0;
    float t2 = DOT(e2, q) * inv;
    if (t2 < 0 || t2 > 1) return 0;
    *tout = t2;
    return 1;
}

int pcol_raycast(Pcol *c, const float o[3], const float d[3], float *t, float min_ny)
{
    float x0 = o[0] < o[0] + d[0] ? o[0] : o[0] + d[0], x1 = o[0] < o[0] + d[0] ? o[0] + d[0] : o[0];
    float z0 = o[2] < o[2] + d[2] ? o[2] : o[2] + d[2], z1 = o[2] < o[2] + d[2] ? o[2] + d[2] : o[2];
    int n = gather(c, x0, z0, x1, z1), best = -1;
    float bt = 2.0f;
    for (int i = 0; i < n; i++) {
        int k = g_cand[i];
        if (c->plane[4 * k + 1] < min_ny) continue;
        float tt;
        c->tests++;
        if (ray_tri(c, k, o, d, &tt) && tt < bt) { bt = tt; best = k; }
    }
    if (best >= 0) *t = bt;
    return best;
}

int pcol_floor(Pcol *c, float x, float z, float y_top, float y_bottom, float min_ny, float *h)
{
    float o[3] = {x, y_top, z}, d[3] = {0, y_bottom - y_top, 0}, t;
    int k = pcol_raycast(c, o, d, &t, min_ny); /* nearest from the top = highest */
    if (k >= 0) *h = y_top + t * (y_bottom - y_top);
    return k;
}

/* Push a sphere out of non-walkable triangles, horizontally. Returns 1 if pushed. */
static int push_sphere(Pcol *c, int n, float ctr[3], float r, float slope_cos)
{
    int hit = 0;
    for (int i = 0; i < n; i++) {
        int k = g_cand[i];
        const float *pl = c->plane + 4 * k;
        float sd = pl[0] * ctr[0] + pl[1] * ctr[1] + pl[2] * ctr[2] - pl[3];
        if (sd > r || sd < -r) continue;
        c->tests++;
        float q[3];
        closest_on_tri(ctr, V(c, k, 0), V(c, k, 1), V(c, k, 2), q);
        float v[3] = {ctr[0] - q[0], ctr[1] - q[1], ctr[2] - q[2]};
        float d2 = DOT(v, v);
        if (d2 >= r * r) continue;
        if (pl[1] >= slope_cos && q[1] <= ctr[1]) continue; /* floor / step top below the sphere: the probe's job */
        if (pl[1] <= -0.7f) continue;                     /* ceilings: no low ones in the arena */
        float hx = v[0], hz = v[2], hl = sqrtf(hx * hx + hz * hz);
        if (hl < 1e-4f) { hx = pl[0]; hz = pl[2]; hl = sqrtf(hx * hx + hz * hz); if (hl < 1e-4f) continue; }
        hx /= hl; hz /= hl;
        /* horizontal distance needed so that the sphere clears q: sqrt(r^2 - dy^2) - current horizontal */
        float dy = v[1], need = sqrtf(r * r - dy * dy) - hl + 0.002f;
        if (sd < 0 && hl < 1e-3f) need = r; /* centre behind the face */
        ctr[0] += hx * need;
        ctr[2] += hz * need;
        hit = 1;
    }
    return hit;
}

int pcol_move(Pcol *c, const PcolCapsule *cap, float pos[3], float dx, float dz, float *vy, float dt, int *floor_tri)
{
    float p[3] = {pos[0] + dx, pos[1], pos[2] + dz};
    float r = cap->radius;
    /* spheres from just above the step height to the head */
    float h0 = cap->step + r, h2 = cap->height - r, hs[3] = {h0, (h0 + h2) * 0.5f, h2};
    float m = r + 0.3f;
    int n = gather(c, p[0] - m, p[2] - m, p[0] + m, p[2] + m);
    for (int it = 0; it < 3; it++) {
        int any = 0;
        for (int s = 0; s < 3; s++) {
            float ctr[3] = {p[0], p[1] + hs[s], p[2]};
            if (push_sphere(c, n, ctr, r, cap->slope_cos)) { p[0] = ctr[0]; p[2] = ctr[2]; any = 1; }
        }
        if (!any) break;
    }
    /* floor: ray from step height above the feet down to snap distance (grounded) or this frame's fall */
    int was_grounded = *vy == 0.0f, grounded = 0;
    if (!was_grounded) *vy -= cap->gravity * dt;
    float fall = *vy * dt < 0 ? *vy * dt : 0;
    float h, bottom = was_grounded ? p[1] - cap->snap_down : p[1] + fall - 0.01f;
    int k = pcol_floor(c, p[0], p[2], p[1] + cap->step, bottom, cap->slope_cos, &h);
    if (k >= 0 && *vy <= 0) {
        p[1] = h; *vy = 0; grounded = 1;
    } else {
        if (was_grounded) *vy = -cap->gravity * dt;
        p[1] += *vy * dt;
        k = -1;
    }
    if (!isfinite(p[0]) || !isfinite(p[1]) || !isfinite(p[2])) { *vy = 0; return 1; } /* never poison the state */
    pos[0] = p[0]; pos[1] = p[1]; pos[2] = p[2];
    if (floor_tri) *floor_tri = k;
    return grounded;
}

static int sphere_hits(Pcol *c, int n, const u8 *ign, const float ctr[3], float r)
{
    for (int i = 0; i < n; i++) {
        if (ign[i]) continue;
        int k = g_cand[i];
        const float *pl = c->plane + 4 * k;
        float sd = pl[0] * ctr[0] + pl[1] * ctr[1] + pl[2] * ctr[2] - pl[3];
        if (sd > r || sd < -r) continue;
        c->tests++;
        float q[3];
        closest_on_tri(ctr, V(c, k, 0), V(c, k, 1), V(c, k, 2), q);
        float v[3] = {ctr[0] - q[0], ctr[1] - q[1], ctr[2] - q[2]};
        if (DOT(v, v) < r * r) return 1;
    }
    return 0;
}

float pcol_sphere_cast(Pcol *c, const float a[3], const float b[3], float r)
{
    static u8 ign[MAX_CAND];
    float d[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
    float len = sqrtf(DOT(d, d));
    if (len < 1e-4f) return 1.0f;
    float x0 = (a[0] < b[0] ? a[0] : b[0]) - r, x1 = (a[0] > b[0] ? a[0] : b[0]) + r;
    float z0 = (a[2] < b[2] ? a[2] : b[2]) - r, z1 = (a[2] > b[2] ? a[2] : b[2]) + r;
    float y0 = (a[1] < b[1] ? a[1] : b[1]) - r, y1 = (a[1] > b[1] ? a[1] : b[1]) + r;
    int n = gather(c, x0, z0, x1, z1), m = 0;
    /* keep only triangles the swept capsule can touch: segment within r of the plane, y range overlaps */
    for (int i = 0; i < n; i++) {
        int k = g_cand[i];
        const float *pl = c->plane + 4 * k;
        float sa = pl[0] * a[0] + pl[1] * a[1] + pl[2] * a[2] - pl[3];
        float sb = pl[0] * b[0] + pl[1] * b[1] + pl[2] * b[2] - pl[3];
        if ((sa > r && sb > r) || (sa < -r && sb < -r)) continue;
        const float *p0 = V(c, k, 0), *p1 = V(c, k, 1), *p2 = V(c, k, 2);
        float ty0 = fminf(p0[1], fminf(p1[1], p2[1])), ty1 = fmaxf(p0[1], fmaxf(p1[1], p2[1]));
        if (ty1 < y0 || ty0 > y1) continue;
        g_cand[m++] = (u16)k;
    }
    /* ignore what the start sphere already touches (pivot hugging a wall must not pin the camera) */
    for (int i = 0; i < m; i++) {
        int k = g_cand[i];
        float q[3];
        closest_on_tri(a, V(c, k, 0), V(c, k, 1), V(c, k, 2), q);
        float v[3] = {a[0] - q[0], a[1] - q[1], a[2] - q[2]};
        ign[i] = DOT(v, v) < r * r;
    }
    int steps = (int)(len / r) + 1;
    float prev = 0;
    for (int s = 1; s <= steps; s++) {
        float t = (float)s / steps, ctr[3] = {a[0] + d[0] * t, a[1] + d[1] * t, a[2] + d[2] * t};
        if (sphere_hits(c, m, ign, ctr, r)) {
            float lo = prev, hi = t;
            for (int it = 0; it < 5; it++) {
                float mid = 0.5f * (lo + hi), q[3] = {a[0] + d[0] * mid, a[1] + d[1] * mid, a[2] + d[2] * mid};
                if (sphere_hits(c, m, ign, q, r)) hi = mid; else lo = mid;
            }
            return lo;
        }
        prev = t;
    }
    return 1.0f;
}
