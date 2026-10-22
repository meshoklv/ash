/*
 * Route planner for the autopilot (native tool, not shipped): A* over the zone's Havok collision with the game's
 * own capsule controller (pcol_move), so a planned step is a step the knight can really take. Doors, gates and fog
 * walls are treated as open (the scenario opens them first); other object boxes (bonfires, the boulder...) block.
 *
 *     asy_path <zone.pak> < points.txt      points: "x y z [label]" per line, FromSoft coordinates
 *
 * Prints, per leg, the waypoints after string-pulling, as C lines for src/game/asy_auto.c (FS coordinates).
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pcol.h"
#include "world.h"

static World W;
static Pcol C;
static const PcolCapsule CAP = {0.35f, 1.8f, 0.45f, 0.64f, 0.35f, 9.8f};
#define CELL 0.5f

/* objects the scenario opens / removes on its way (cell door, big door, gate, east wing door, back door, fog walls,
 * the iron ball's wall); every other door stays closed and blocks */
static const u32 OPENED[] = {1811103, 1811111, 1811115, 1811106, 1811110, 1811700, 1811990};

static u32 g_extra_open[16];
static int g_n_extra;

static void obj_push(float p[3])
{
    for (u32 i = 0; i < W.z->n_obj; i++) {
        int k = W.obj[i].kind, open = k == WK_FOG || k == WK_BOULDER || k == WK_LADDER;
        for (int j = 0; j < g_n_extra; j++) if (W.obj[i].entity == g_extra_open[j]) open = 1;
        for (unsigned j = 0; j < sizeof OPENED / sizeof OPENED[0]; j++) if (W.obj[i].entity == OPENED[j]) open = 1;
        W.obj_on[i] = !open;
    }
    world_obj_push(&W, p, CAP.radius, CAP.height);
}

/* walk from a toward b (xz) with the real controller; returns 1 if it arrives (within 0.12 m) without falling */
static int walk(const float a[3], const float b[3], float out[3])
{
    float p[3] = {a[0], a[1], a[2]}, vy = 0;
    float dx = b[0] - a[0], dz = b[2] - a[2], d = sqrtf(dx * dx + dz * dz);
    int n = (int)(d / 0.12f) + 1, tri;
    for (int i = 0; i < n; i++) {
        float y0 = p[1];
        int gr = pcol_move(&C, &CAP, p, dx / n, dz / n, &vy, 1.0f / 30, &tri);
        obj_push(p);
        (void)gr;
        if (p[1] < a[1] - 1.3f) return 0;   /* small drops (steps down, ledges < 1.3 m) are fine */
    }
    int gr2 = 0;
    for (int k = 0; k < 20 && !gr2; k++) gr2 = pcol_move(&C, &CAP, p, 0, 0, &vy, 1.0f / 30, &tri);
    if (!gr2 || p[1] < a[1] - 1.3f) return 0;
    if (out) memcpy(out, p, 12);
    float ex = p[0] - b[0], ez = p[2] - b[2];
    return ex * ex + ez * ez < 0.12f * 0.12f && fabsf(p[1] - b[1]) < 1.4f;
}

typedef struct { int ix, iz, iy; float p[3]; float g, f; int parent, open; } Node;
static Node *N;
static int nn, cap_n;
static int *H;
#define HN (1 << 20)

static unsigned hkey(int ix, int iz, int iy) { return ((unsigned)ix * 73856093u ^ (unsigned)iz * 19349663u ^ (unsigned)iy * 83492791u) & (HN - 1); }
static int *HNEXT;

static int node_get(const float p[3], int create)
{
    int ix = (int)floorf(p[0] / CELL + 0.5f), iz = (int)floorf(p[2] / CELL + 0.5f), iy = (int)floorf(p[1] / 0.75f + 0.5f);
    unsigned h = hkey(ix, iz, iy);
    for (int i = H[h]; i >= 0; i = HNEXT[i])
        if (N[i].ix == ix && N[i].iz == iz && abs(N[i].iy - iy) <= 0) return i;
    if (!create) return -1;
    if (nn == cap_n) { cap_n = cap_n ? cap_n * 2 : 65536; N = realloc(N, sizeof(Node) * cap_n); HNEXT = realloc(HNEXT, sizeof(int) * cap_n); }
    Node *n = &N[nn];
    memset(n, 0, sizeof *n);
    n->ix = ix; n->iz = iz; n->iy = iy;
    memcpy(n->p, p, 12);
    n->g = 1e30f; n->parent = -1;
    HNEXT[nn] = H[h];
    H[h] = nn;
    return nn++;
}

/* binary heap of node indices by f */
static int *heap, hn, hcap;
static void hpush(int i)
{
    if (hn == hcap) { hcap = hcap ? hcap * 2 : 65536; heap = realloc(heap, sizeof(int) * hcap); }
    int k = hn++;
    heap[k] = i;
    while (k > 0) { int p = (k - 1) / 2; if (N[heap[p]].f <= N[heap[k]].f) break; int t = heap[p]; heap[p] = heap[k]; heap[k] = t; k = p; }
}
static int hpop(void)
{
    int r = heap[0];
    heap[0] = heap[--hn];
    int k = 0;
    for (;;) {
        int l = 2 * k + 1, m = k;
        if (l < hn && N[heap[l]].f < N[heap[m]].f) m = l;
        if (l + 1 < hn && N[heap[l + 1]].f < N[heap[m]].f) m = l + 1;
        if (m == k) break;
        int t = heap[m]; heap[m] = heap[k]; heap[k] = t; k = m;
    }
    return r;
}

static float dist3(const float a[3], const float b[3])
{
    float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return sqrtf(dx * dx + dy * dy * 4 + dz * dz);
}

static void snap(float p[3])
{
    float h;
    int t = pcol_floor(&C, p[0], p[2], p[1] + 1.0f, p[1] - 3.0f, CAP.slope_cos, &h);
    if (t >= 0) p[1] = h;
}

static int plan(const float a0[3], const float b0[3], float (*out)[3], int max_out)
{
    float a[3], b[3];
    memcpy(a, a0, 12); memcpy(b, b0, 12);
    snap(a); snap(b);
    nn = 0; hn = 0;
    memset(H, -1, sizeof(int) * HN);
    int s = node_get(a, 1);
    N[s].g = 0; N[s].f = dist3(a, b);
    hpush(s);
    int goal = -1, it = 0;
    static const int DX[8] = {1, -1, 0, 0, 1, 1, -1, -1}, DZ[8] = {0, 0, 1, -1, 1, -1, 1, -1};
    while (hn && it < 600000) {
        int i = hpop();
        it++;
        if (N[i].open < 0) continue;
        N[i].open = -1;
        float dx = N[i].p[0] - b[0], dz = N[i].p[2] - b[2];
        if (dx * dx + dz * dz < 0.6f * 0.6f && fabsf(N[i].p[1] - b[1]) < 1.0f) { goal = i; break; }
        /* direct finish */
        if (dx * dx + dz * dz < 1.5f * 1.5f) {
            float q[3];
            if (walk(N[i].p, b, q)) { int j = node_get(q, 1); if (N[j].g > N[i].g + 1) { N[j].g = N[i].g + 1; N[j].parent = i; N[j].f = N[j].g; hpush(j); } }
        }
        for (int k = 0; k < 8; k++) {
            float t[3] = {(N[i].ix + DX[k]) * CELL, N[i].p[1], (N[i].iz + DZ[k]) * CELL}, q[3];
            if (!walk(N[i].p, t, q)) continue;
            int j = node_get(q, 1);
            float c = N[i].g + sqrtf((float)(DX[k] * DX[k] + DZ[k] * DZ[k])) * CELL;
            if (c < N[j].g) { N[j].g = c; N[j].parent = i; N[j].f = c + dist3(q, b); N[j].open = 0; hpush(j); }
        }
    }
    if (goal < 0) {
        int bi = 0;
        for (int i = 1; i < nn; i++) if (dist3(N[i].p, b) < dist3(N[bi].p, b)) bi = i;
        fprintf(stderr, "  no path (%d iterations, %d nodes), closest FS(%.2f %.2f %.2f), target FS(%.2f %.2f %.2f)\n", it,
                nn, -N[bi].p[0], N[bi].p[1], N[bi].p[2], -b[0], b[1], b[2]);
        return 0;
    }
    /* path back */
    int path[8192], np = 0;
    for (int i = goal; i >= 0 && np < 8192; i = N[i].parent) path[np++] = i;
    /* string pulling with the real controller */
    int n_out = 0, cur = np - 1;
    float pos[3];
    memcpy(pos, N[path[cur]].p, 12);
    while (cur > 0 && n_out < max_out) {
        int best = cur - 1;
        for (int j = 0; j < cur; j++) { if (walk(pos, N[path[j]].p, NULL)) { best = j; break; } }
        memcpy(out[n_out++], N[path[best]].p, 12);
        memcpy(pos, N[path[best]].p, 12);
        cur = best;
    }
    fprintf(stderr, "  path: %d nodes expanded, %d grid steps, %d waypoints\n", it, np, n_out);
    return n_out;
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "asy_path zone.pak < points\n"); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 2;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    u8 *buf;
    if (posix_memalign((void **)&buf, 64, n + 64)) return 2;
    if (fread(buf, 1, n, f) != (size_t)n) return 2;
    fclose(f);
    if (world_load(&W, buf, (int)n) || pcol_load(&C, W.pcol, W.pcol_size)) { fprintf(stderr, "load failed\n"); return 1; }
    H = malloc(sizeof(int) * HN);
    pcol_part_enable(&C, pcol_part_find(&C, "h0550B1_0000"), 0);   /* gate open */
    if (argc >= 3 && !strncmp(argv[2], "open=", 5))   /* more objects open: open=1811210,1811211 */
        for (char *t = strtok(argv[2] + 5, ","); t && g_n_extra < 16; t = strtok(NULL, ",")) g_extra_open[g_n_extra++] = (u32)atol(t);
    if (argc >= 8 && !strcmp(argv[2], "walk")) {   /* debug: asy_path pak walk x y z x y z (FS) */
        float a[3] = {-(float)atof(argv[3]), (float)atof(argv[4]), (float)atof(argv[5])};
        float b[3] = {-(float)atof(argv[6]), (float)atof(argv[7]), (float)atof(argv[8])};
        snap(a);
        float p[3] = {a[0], a[1], a[2]}, vy = 0;
        float dx = b[0] - a[0], dz = b[2] - a[2], d = sqrtf(dx * dx + dz * dz);
        int n2 = (int)(d / 0.12f) + 1, tri;
        for (int i = 0; i < n2; i++) {
            int gr = pcol_move(&C, &CAP, p, dx / n2, dz / n2, &vy, 1.0f / 30, &tri);
            printf("%3d FS(%.2f %.2f %.2f) grounded %d tri %d part %s\n", i, -p[0], p[1], p[2], gr, tri,
                   tri >= 0 ? C.part[C.tri[tri].part].name : "-");
        }
        return 0;
    }
    char line[256];
    float prev[3];
    int have = 0, k_last = 0;
    static float out[512][3];
    while (fgets(line, sizeof line, stdin)) {
        float x, y, z;
        char lab[128] = "";
        if (line[0] == '#' || sscanf(line, "%f %f %f %127[^\n]", &x, &y, &z, lab) < 3) continue;
        float p[3] = {-x, y, z};
        if (have) {
            fprintf(stderr, "leg -> %s\n", lab);
            int k = plan(prev, p, out, 512);
            k_last = k;
            for (int i = 0; i < k; i++)
                printf("    {A_GOTO, %.2ff, %.2ff, %.2ff, 0, \"%s\"},\n", -out[i][0], out[i][1], out[i][2],
                       i == k - 1 ? lab : "");
            if (!k) printf("    /* NO PATH */ {A_GOTO, %.2ff, %.2ff, %.2ff, 0, \"%s\"},\n", x, y, z, lab);
        }
        memcpy(prev, p, 12);
        snap(prev);
        if (have && k_last > 0) memcpy(prev, out[k_last - 1], 12);   /* start the next leg where this one ended */
        have = 1;
    }
    return 0;
}
