/*
 * Ash bench (milestone M2, PLAN.md §6.6 / §7.1): numbers that only a real PSP can give.
 *
 * Sections (in run order):
 *   sys    model, firmware, free RAM after init (sceKernelMaxFreeMemSize), boot path, memory mode
 *   cpu    clock 222 vs 333, matrix multiply FPU vs VFPU, 32-bone pose sampling
 *   ms     Memory Stick write/read speed (8 MB temp file next to the EBOOT)
 *   tri    GE ms per N thousand triangles: no texture / CLUT4 / CLUT8 / CLUT8 in VRAM / 16-bit 5650,
 *          Z on/off, indexed / non-indexed / strips, float vertices
 *   fill   overdraw 1x/2x/4x full-screen quads (flat, CLUT4, CLUT4+blend)
 *   open   10k-triangle "open view" (CLUT4, overdraw 2x) -> GATE2; + second (lightmap) pass
 *   skin   N characters x 2048 tris x 32 bones: A8 (8-bone chunks, GU_WEIGHTS(8)), A4, C (rigid)  -> GATE1
 *   gate3  cooked scene (../ASHVIEW/scene.ashm, else synthetic 2.7 MB) vs free RAM            -> GATE3
 *
 * GE timing: a display list is pre-built with GU_CALL; the timed part is
 * sceGuStart(DIRECT) + sceGuCallList + sceGuFinish + sceGuSync, i.e. pure GE time.
 * Results: on screen (big summary, L/R = detail pages) and <EBOOT dir>/results.txt
 * (rewritten after every section, so a crash still leaves partial results).
 *
 * ASH_MEM_ONLY build (dist/ASHMEM64, PARAM.SFO MEMSIZE=1): only sys + gate3, to see the 64 MB mode.
 *
 * Emulator numbers are NOT hardware numbers (PLAN §6.2). The bench flags "EMULATOR?" heuristically.
 */
#include <pspkernel.h>
#include <pspdisplay.h>
#include <pspctrl.h>
#include <psprtc.h>
#include <psppower.h>
#include <pspge.h>
#include <pspgu.h>
#include <pspgum.h>
#include <kubridge.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <malloc.h>
#include <math.h>

#ifdef ASH_MEM_ONLY
PSP_MODULE_INFO("Ash Mem64", 0, 1, 0);
#else
PSP_MODULE_INFO("Ash Bench", 0, 1, 0);
#endif
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);
#define HEAP_KB 4096
PSP_HEAP_SIZE_KB(HEAP_KB);

#include "diag.h"

#ifndef ASH_GIT_SHA
#define ASH_GIT_SHA "nogit"
#endif
#define BENCH_VERSION "1"

#define BUF_W 512
#define SCR_W 480
#define SCR_H 272

/* §7.1 thresholds and §4.1 numbers used by GATE3 */
#define PERM_KB      (4608)          /* perm upper bound 4.5 MB (§4.1 table) */
#define RESERVE_KB   (2048)          /* build gate reserve 2 MB */
#define SYNTH_SCENE  (2756704)       /* bytes of the cooked hall.ashm on 2026-10-14 (synthetic fallback) */

/* ------------------------------------------------------------------ log */
static char g_log[64 * 1024];
static int g_loglen;
static char g_dir[256] = "ms0:/PSP/GAME/ASHBENCH";
static char g_boot[256] = "?";
static char g_saved[300] = "not saved yet";
static int g_emu_guess;

static void blog(const char *fmt, ...)
{
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    printf("ash-bench: %s\n", line);
    diag_event("%s", line);   /* log.txt (src/diag): written out once a second, survives a freeze */
    int n = strlen(line);
    if (g_loglen + n + 2 < (int)sizeof(g_log)) {
        memcpy(g_log + g_loglen, line, n);
        g_loglen += n;
        g_log[g_loglen++] = '\n';
        g_log[g_loglen] = 0;
    }
}

/* summary lines for the big screen */
#define SUM_MAX 14
static char g_sum[SUM_MAX][40];
static unsigned short g_sumcol[SUM_MAX];
static int g_nsum;
static void suml(unsigned short col, const char *fmt, ...)
{
    if (g_nsum >= SUM_MAX) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_sum[g_nsum], sizeof(g_sum[0]), fmt, ap);
    va_end(ap);
    g_sumcol[g_nsum++] = col;
}

static int write_file(const char *path, const void *data, int len)
{
    SceUID fd = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0) return fd;
    int w = sceIoWrite(fd, data, len);
    sceIoClose(fd);
    return w == len ? 0 : -1;
}

static void save_results(void)
{
    char path[300];
    snprintf(path, sizeof(path), "%s/results.txt", g_dir);
    if (write_file(path, g_log, g_loglen) == 0) { snprintf(g_saved, sizeof(g_saved), "%s", path); return; }
    const char *fb = "ms0:/ASHBENCH_results.txt";
    if (write_file(fb, g_log, g_loglen) == 0) { snprintf(g_saved, sizeof(g_saved), "%s", fb); return; }
    snprintf(g_saved, sizeof(g_saved), "FAILED to write %s", path);
}

static inline u64 now_us(void) { return (u64)sceKernelGetSystemTimeWide(); }

/* ------------------------------------------------------------------ callbacks */
static int exit_cb(int a, int b, void *c) { (void)a; (void)b; (void)c; sceKernelExitGame(); return 0; }
static int cb_thread(SceSize args, void *argp)
{
    (void)args; (void)argp;
    int cbid = sceKernelCreateCallback("Exit Callback", exit_cb, NULL);
    sceKernelRegisterExitCallback(cbid);
    sceKernelSleepThreadCB();
    return 0;
}

/* ------------------------------------------------------------------ text (msx 8x8 font from libpspdebug) */
extern unsigned char msx[];
#define C_WHITE  0xFFFF
#define C_GREY   0x8410
#define C_RED    0x001F   /* 5650: R in low bits */
#define C_GREEN  0x07E0
#define C_YELLOW 0x07FF
#define C_CYAN   0xFFE0

static void *g_fb[2];
static void *g_zb;
static void *g_draw;       /* VRAM-relative pointer of the current draw buffer */

static unsigned short *fb_ptr(void *rel) { return (unsigned short *)(0x44000000 | (u32)rel); }

static void text(unsigned short *fb, int x, int y, int sc, unsigned short col, const char *s)
{
    for (; *s; s++, x += 8 * sc) {
        if (x + 8 * sc > SCR_W) break;
        const unsigned char *g = msx + (unsigned char)*s * 8;
        for (int r = 0; r < 8; r++) {
            unsigned char bits = g[r];
            if (!bits) continue;
            for (int c = 0; c < 8; c++) {
                if (!(bits & (0x80 >> c))) continue;
                for (int yy = 0; yy < sc; yy++) {
                    int py = y + r * sc + yy;
                    if (py >= SCR_H) break;
                    unsigned short *p = fb + py * BUF_W + x + c * sc;
                    for (int xx = 0; xx < sc; xx++) p[xx] = col;
                }
            }
        }
    }
}

static void fill_rect(unsigned short *fb, int x, int y, int w, int h, unsigned short col)
{
    for (int j = y; j < y + h && j < SCR_H; j++)
        for (int i = x; i < x + w && i < SCR_W; i++) fb[j * BUF_W + i] = col;
}

/* ------------------------------------------------------------------ GU */
static unsigned int __attribute__((aligned(64))) g_list[64 * 1024];
static unsigned int __attribute__((aligned(64))) g_call[64 * 1024];
static unsigned int __attribute__((aligned(64))) g_call_world[16 * 1024];

static void gu_init(void)
{
    g_fb[0] = guGetStaticVramBuffer(BUF_W, SCR_H, GU_PSM_5650);
    g_fb[1] = guGetStaticVramBuffer(BUF_W, SCR_H, GU_PSM_5650);
    g_zb = guGetStaticVramBuffer(BUF_W, SCR_H, GU_PSM_4444);
    sceGuInit();
    sceGuStart(GU_DIRECT, g_list);
    sceGuDrawBuffer(GU_PSM_5650, g_fb[0], BUF_W);
    sceGuDispBuffer(SCR_W, SCR_H, g_fb[1], BUF_W);
    sceGuDepthBuffer(g_zb, BUF_W);
    sceGuOffset(2048 - (SCR_W / 2), 2048 - (SCR_H / 2));
    sceGuViewport(2048, 2048, SCR_W, SCR_H);
    sceGuDepthRange(65535, 0);
    sceGuScissor(0, 0, SCR_W, SCR_H);
    sceGuEnable(GU_SCISSOR_TEST);
    sceGuDepthFunc(GU_GEQUAL);
    sceGuEnable(GU_DEPTH_TEST);
    sceGuFrontFace(GU_CW);
    sceGuShadeModel(GU_SMOOTH);
    sceGuDisable(GU_CULL_FACE);
    sceGuEnable(GU_CLIP_PLANES);
    sceGuEnable(GU_DITHER);
    sceGuFinish();
    sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);
    sceDisplayWaitVblankStart();
    sceGuDisplay(GU_TRUE);
    g_draw = g_fb[0];
}

static void gu_clear(unsigned int abgr)
{
    sceGuStart(GU_DIRECT, g_list);
    sceGuClearColor(abgr);
    sceGuClearDepth(0);
    sceGuClear(GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT);
    sceGuFinish();
    diag_ge_wait("bench clear", 3000, 1);
}

/* draw a status line into the current draw buffer, show it, keep the PSP awake */
static void present(const char *what, int vsync)
{
    unsigned short *fb = fb_ptr(g_draw);
    fill_rect(fb, 0, 0, SCR_W, 20, 0x0000);
    text(fb, 4, 2, 2, C_YELLOW, what);
    fill_rect(fb, 0, SCR_H - 12, SCR_W, 12, 0x0000);
    text(fb, 4, SCR_H - 10, 1, C_GREY, g_emu_guess ? "Ash Bench  [EMULATOR?]  HOME = quit" : "Ash Bench  HOME = quit");
    if (vsync) sceDisplayWaitVblankStart();
    g_draw = sceGuSwapBuffers();
    scePowerTick(PSP_POWER_TICK_ALL);
    diag_beat();
}

/* GE time of a pre-built call list. Returns avg ms over reps (first rep = warm-up, skipped). */
typedef struct { float avg, min; } GeTime;

static int g_ge_test;
static GeTime ge_time_call(const void *call, int reps, int clear_each)
{
    GeTime r = {0, 1e9f};
    int n = 0;
    diag_event_sync("GE test #%d begin", ++g_ge_test);   /* the result line names it; a freeze leaves this as the last */
    for (int i = 0; i <= reps; i++) {
        if (clear_each) gu_clear(0xff302020);
        u64 t0 = now_us();
        sceGuStart(GU_DIRECT, g_list);
        sceGuCallList(call);
        sceGuFinish();
        int hung = diag_ge_wait("bench GE test", 3000, 1);   /* busy poll: exact time; >3 s -> logged + GE reset */
        u64 t1 = now_us();
        diag_beat();
        if (hung) { blog("GE HANG in test #%d (GE reset, test skipped)", g_ge_test); r.avg = r.min = -1; return r; }
        float ms = (t1 - t0) / 1000.0f;
        if (i == 0) continue;
        r.avg += ms; n++;
        if (ms < r.min) r.min = ms;
    }
    r.avg /= n;
    return r;
}

/* ------------------------------------------------------------------ textures */
static unsigned char __attribute__((aligned(16))) g_tex4[128 * 128 / 2];
static unsigned char __attribute__((aligned(16))) g_tex8[128 * 128];
static unsigned short __attribute__((aligned(16))) g_tex16[128 * 128];
static unsigned short __attribute__((aligned(16))) g_clut4[16];
static unsigned short __attribute__((aligned(16))) g_clut8[256];
static void *g_tex8_vram;

static void swizzle(unsigned char *out, const unsigned char *in, unsigned int wbytes, unsigned int h)
{
    unsigned int src_pitch = (wbytes - 16) / 4, src_row = wbytes * 8;
    const unsigned char *ysrc = in;
    u32 *dst = (u32 *)out;
    for (unsigned int by = 0; by < h / 8; by++) {
        const unsigned char *xsrc = ysrc;
        for (unsigned int bx = 0; bx < wbytes / 16; bx++) {
            const u32 *src = (const u32 *)xsrc;
            for (int j = 0; j < 8; j++) {
                *dst++ = *src++; *dst++ = *src++; *dst++ = *src++; *dst++ = *src++;
                src += src_pitch;
            }
            xsrc += 16;
        }
        ysrc += src_row;
    }
}

static unsigned short rgb565(int r, int g, int b) { return (unsigned short)((r >> 3) | ((g >> 2) << 5) | ((b >> 3) << 11)); }

static void make_textures(void)
{
    static unsigned char lin[128 * 128 * 2];
    for (int i = 0; i < 16; i++) g_clut4[i] = rgb565(40 + i * 12, 30 + i * 9, 20 + i * 6);
    for (int i = 0; i < 256; i++) g_clut8[i] = rgb565(i, (i * 3) & 255, 255 - i);
    /* T4: two texels per byte, low nibble first */
    for (int y = 0; y < 128; y++)
        for (int x = 0; x < 128; x += 2) {
            int a = ((((x >> 3) ^ (y >> 3)) & 1) ? 12 : 3) + ((x * 7 + y * 13) & 3);
            int b = ((((x + 1) >> 3 ^ (y >> 3)) & 1) ? 12 : 3) + (((x + 1) * 7 + y * 13) & 3);
            lin[y * 64 + x / 2] = (unsigned char)(a | (b << 4));
        }
    swizzle(g_tex4, lin, 64, 128);
    for (int y = 0; y < 128; y++)
        for (int x = 0; x < 128; x++) lin[y * 128 + x] = (unsigned char)((x ^ y) + ((x >> 4) * 16));
    swizzle(g_tex8, lin, 128, 128);
    unsigned short *l16 = (unsigned short *)lin;
    for (int y = 0; y < 128; y++)
        for (int x = 0; x < 128; x++) l16[y * 128 + x] = rgb565(x * 2, y * 2, ((x ^ y) & 32) ? 200 : 60);
    swizzle((unsigned char *)g_tex16, lin, 256, 128);
    /* VRAM copy of the CLUT8 texture, after the 3 frame buffers */
    /* guGetStaticVramTexture already returns an ABSOLUTE VRAM address (sceGeEdramGetAddr() + offset, pspgu of
       pspdev v20261001 [проверено: disassembly]). Adding sceGeEdramGetAddr() again gave 0x080CC000 = KERNEL RAM:
       the memcpy below then wrote 16 KB into kernel memory through 0x480CC000 -> exception / freeze on the real PSP
       right after the "ms" section (PPSSPP does not protect kernel RAM). docs/hw-pitfalls.md */
    g_tex8_vram = guGetStaticVramTexture(128, 128, GU_PSM_T8);
    memcpy((void *)(0x40000000 | (u32)g_tex8_vram), g_tex8, sizeof(g_tex8));
    sceKernelDcacheWritebackAll();
}

enum { TX_NONE, TX_CLUT4, TX_CLUT8, TX_CLUT8_VRAM, TX_565 };
static const char *tx_name[] = {"notex", "clut4", "clut8", "clut8vram", "565"};

static void bind_tex(int tx)
{
    if (tx == TX_NONE) { sceGuDisable(GU_TEXTURE_2D); return; }
    sceGuEnable(GU_TEXTURE_2D);
    sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGB);
    sceGuTexFilter(GU_LINEAR, GU_LINEAR);
    sceGuTexWrap(GU_REPEAT, GU_REPEAT);
    sceGuTexOffset(0, 0);
    switch (tx) {
    case TX_CLUT4:
        sceGuClutMode(GU_PSM_5650, 0, 0xff, 0);
        sceGuClutLoad(2, g_clut4);
        sceGuTexMode(GU_PSM_T4, 0, 0, 1);
        sceGuTexImage(0, 128, 128, 128, g_tex4);
        break;
    case TX_CLUT8:
    case TX_CLUT8_VRAM:
        sceGuClutMode(GU_PSM_5650, 0, 0xff, 0);
        sceGuClutLoad(32, g_clut8);
        sceGuTexMode(GU_PSM_T8, 0, 0, 1);
        sceGuTexImage(0, 128, 128, 128, tx == TX_CLUT8 ? (void *)g_tex8 : g_tex8_vram);
        break;
    case TX_565:
        sceGuTexMode(GU_PSM_5650, 0, 0, 1);
        sceGuTexImage(0, 128, 128, 128, g_tex16);
        break;
    }
}

/* ------------------------------------------------------------------ grid meshes (pixel*16 coords, uv = pixel*64) */
typedef struct { unsigned short u, v; short x, y, z; } VUV;      /* 10 B */
typedef struct { short x, y, z; } VP;                             /* 6 B  */
typedef struct { float u, v, x, y, z; } VF;                       /* 20 B */

typedef struct {
    int cols, rows, nv, ntri;
    VUV *vuv; VP *vp; VF *vf;
    unsigned short *idx;      /* triangles */
    unsigned short *sidx;     /* strips joined with degenerates */
    int sidx_per_row;
    VUV *flat;                /* non-indexed expansion (optional) */
} Grid;

static void grid_make(Grid *g, int cols, int rows, float x0, float y0, float w, float h, int want_float, int want_flat)
{
    g->cols = cols; g->rows = rows;
    g->nv = (cols + 1) * (rows + 1);
    g->ntri = cols * rows * 2;
    g->vuv = memalign(16, g->nv * sizeof(VUV));
    g->vp = memalign(16, g->nv * sizeof(VP));
    g->vf = want_float ? memalign(16, g->nv * sizeof(VF)) : NULL;
    for (int r = 0; r <= rows; r++)
        for (int c = 0; c <= cols; c++) {
            int i = r * (cols + 1) + c;
            float px = x0 + w * c / cols, py = y0 + h * r / rows;
            g->vuv[i].u = (unsigned short)(px * 64); g->vuv[i].v = (unsigned short)(py * 64);
            g->vuv[i].x = (short)(px * 16); g->vuv[i].y = (short)(py * 16); g->vuv[i].z = 0;
            g->vp[i].x = g->vuv[i].x; g->vp[i].y = g->vuv[i].y; g->vp[i].z = 0;
            if (g->vf) { g->vf[i].u = px / 128.0f; g->vf[i].v = py / 128.0f; g->vf[i].x = px; g->vf[i].y = py; g->vf[i].z = 0; }
        }
    g->idx = memalign(16, g->ntri * 3 * 2);
    int k = 0;
    for (int r = 0; r < rows; r++)
        for (int c = 0; c < cols; c++) {
            unsigned short a = r * (cols + 1) + c, b = a + 1, d = a + cols + 1, e = d + 1;
            g->idx[k++] = a; g->idx[k++] = d; g->idx[k++] = b;
            g->idx[k++] = b; g->idx[k++] = d; g->idx[k++] = e;
        }
    g->sidx_per_row = 2 * (cols + 1) + 2;
    g->sidx = memalign(16, rows * g->sidx_per_row * 2);
    k = 0;
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c <= cols; c++) {
            g->sidx[k++] = r * (cols + 1) + c;
            g->sidx[k++] = (r + 1) * (cols + 1) + c;
        }
        g->sidx[k] = g->sidx[k - 1]; k++;                         /* degenerate join */
        g->sidx[k++] = (r + 1) * (cols + 1);
    }
    g->flat = NULL;
    if (want_flat) {
        g->flat = memalign(16, g->ntri * 3 * sizeof(VUV));
        for (int i = 0; i < g->ntri * 3; i++) g->flat[i] = g->vuv[g->idx[i]];
    }
    sceKernelDcacheWritebackAll();
}

static ScePspFMatrix4 g_proj_ortho, g_ident, g_model_px16;

static void set_ortho(int px16)
{
    sceGuSetMatrix(GU_PROJECTION, &g_proj_ortho);
    sceGuSetMatrix(GU_VIEW, &g_ident);
    sceGuSetMatrix(GU_MODEL, px16 ? &g_model_px16 : &g_ident);
}

enum { P_IDX, P_FLAT, P_STRIP, P_FLOAT };
#define DRAW_MAX_TRI 20000   /* 60000 vertices per draw call */
static const char *p_name[] = {"idx", "nonidx", "strip", "float"};

/* append draw of the first `ntri` triangles of grid g to the current list */
static void grid_draw(const Grid *g, int tx, int prim, int ntri)
{
    int tex16 = tx != TX_NONE;
    sceGuTexScale(4.0f, 4.0f);      /* u16 uv = px*64 -> uv = px/512 -> *4*128 = 1 texel per pixel */
    switch (prim) {
    case P_IDX:
        set_ortho(1);
        for (int s = 0; s < ntri; s += DRAW_MAX_TRI)  /* vertex count field is 16-bit */
            sceGuDrawArray(GU_TRIANGLES, (tex16 ? GU_TEXTURE_16BIT : 0) | GU_VERTEX_16BIT | GU_INDEX_16BIT | GU_TRANSFORM_3D,
                           (ntri - s < DRAW_MAX_TRI ? ntri - s : DRAW_MAX_TRI) * 3, g->idx + s * 3,
                           tex16 ? (void *)g->vuv : (void *)g->vp);
        break;
    case P_FLAT:
        set_ortho(1);
        for (int s = 0; s < ntri; s += DRAW_MAX_TRI)
            sceGuDrawArray(GU_TRIANGLES, GU_TEXTURE_16BIT | GU_VERTEX_16BIT | GU_TRANSFORM_3D,
                           (ntri - s < DRAW_MAX_TRI ? ntri - s : DRAW_MAX_TRI) * 3, NULL, g->flat + s * 3);
        break;
    case P_STRIP: {
        set_ortho(1);
        int rows = ntri / (2 * g->cols);
        sceGuDrawArray(GU_TRIANGLE_STRIP, GU_TEXTURE_16BIT | GU_VERTEX_16BIT | GU_INDEX_16BIT | GU_TRANSFORM_3D,
                       rows * g->sidx_per_row - 2, g->sidx, g->vuv);
        break;
    }
    case P_FLOAT:
        set_ortho(0);
        sceGuTexScale(1.0f, 1.0f);
        for (int s = 0; s < ntri; s += DRAW_MAX_TRI)
            sceGuDrawArray(GU_TRIANGLES, GU_TEXTURE_32BITF | GU_VERTEX_32BITF | GU_INDEX_16BIT | GU_TRANSFORM_3D,
                           (ntri - s < DRAW_MAX_TRI ? ntri - s : DRAW_MAX_TRI) * 3, g->idx + s * 3, g->vf);
        break;
    }
}

static void common_state(int ztest)
{
    if (ztest) { sceGuEnable(GU_DEPTH_TEST); sceGuDepthFunc(GU_GEQUAL); sceGuDepthMask(GU_FALSE); }
    else sceGuDisable(GU_DEPTH_TEST);
    sceGuDisable(GU_BLEND);
    sceGuDisable(GU_LIGHTING);
    sceGuDisable(GU_CULL_FACE);
    sceGuColor(0xffffffff);
}

/* ------------------------------------------------------------------ CPU tests */
static void mat_mul_fpu(float *r, const float *a, const float *b)
{
    float t[16];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            t[i * 4 + j] = a[0 * 4 + j] * b[i * 4 + 0] + a[1 * 4 + j] * b[i * 4 + 1] + a[2 * 4 + j] * b[i * 4 + 2] + a[3 * 4 + j] * b[i * 4 + 3];
    memcpy(r, t, sizeof(t));
}

typedef struct { float x, y, z, w; } Quat;

static void quat_to_mat(float *m, Quat q, float tx, float ty, float tz)
{
    float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z, xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z, wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
    m[0] = 1 - 2 * (yy + zz); m[1] = 2 * (xy + wz);     m[2] = 2 * (xz - wy);     m[3] = 0;
    m[4] = 2 * (xy - wz);     m[5] = 1 - 2 * (xx + zz); m[6] = 2 * (yz + wx);     m[7] = 0;
    m[8] = 2 * (xz + wy);     m[9] = 2 * (yz - wx);     m[10] = 1 - 2 * (xx + yy); m[11] = 0;
    m[12] = tx; m[13] = ty; m[14] = tz; m[15] = 1;
}

static volatile float g_sink;

typedef struct { float mm_fpu_ns, mm_vfpu_ns, pose_us, loop_ms; int cpu, bus; } CpuRes;

static void cpu_tests(CpuRes *res)
{
    static ScePspFMatrix4 a, b, c;
    gumLoadIdentity(&a); gumLoadIdentity(&b);
    { ScePspFVector3 v = {1, 2, 3}; gumTranslate(&a, &v); gumRotateX(&b, 0.3f); }
    const int N = 20000;
    res->cpu = scePowerGetCpuClockFrequencyInt();
    res->bus = scePowerGetBusClockFrequencyInt();

    u64 t0 = now_us();
    for (int i = 0; i < N; i++) { mat_mul_fpu((float *)&c, (float *)&a, (float *)&b); b.w.x = c.w.x * 0.5f; }
    u64 t1 = now_us();
    res->mm_fpu_ns = (t1 - t0) * 1000.0f / N;

    gumLoadIdentity(&b); gumRotateX(&b, 0.3f);
    t0 = now_us();
    for (int i = 0; i < N; i++) { gumMultMatrix(&c, &a, &b); b.w.x = c.w.x * 0.5f; }
    t1 = now_us();
    res->mm_vfpu_ns = (t1 - t0) * 1000.0f / N;
    g_sink = c.w.x;

    /* pose sampling, 32 bones: nlerp of two keys, quat->matrix, parent multiply (FPU, as gameplay code would) */
    static float world[32][16] __attribute__((aligned(16)));
    Quat k0[32], k1[32];
    for (int i = 0; i < 32; i++) {
        k0[i] = (Quat){0.1f * i, 0.2f, 0.0f, 1.0f};
        k1[i] = (Quat){0.0f, 0.1f * i, 0.3f, 0.9f};
    }
    const int CH = 1000;
    t0 = now_us();
    for (int n = 0; n < CH; n++) {
        float t = (n & 63) / 63.0f;
        for (int i = 0; i < 32; i++) {
            Quat q = {k0[i].x + (k1[i].x - k0[i].x) * t, k0[i].y + (k1[i].y - k0[i].y) * t,
                      k0[i].z + (k1[i].z - k0[i].z) * t, k0[i].w + (k1[i].w - k0[i].w) * t};
            float il = 1.0f / sqrtf(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
            q.x *= il; q.y *= il; q.z *= il; q.w *= il;
            float local[16];
            quat_to_mat(local, q, 0.0f, 0.1f, 0.0f);
            if (i == 0) memcpy(world[0], local, sizeof(local));
            else mat_mul_fpu(world[i], world[(i - 1) / 2], local);
        }
    }
    t1 = now_us();
    res->pose_us = (float)(t1 - t0) / CH;
    g_sink = world[31][12];

    /* integer loop: should scale ~1.5x between 222 and 333 */
    volatile unsigned int acc = 0;
    t0 = now_us();
    for (unsigned int i = 0; i < 5000000; i++) acc += i ^ (acc >> 3);
    t1 = now_us();
    res->loop_ms = (t1 - t0) / 1000.0f;
}

/* ------------------------------------------------------------------ skinned characters */
#define NB 32
typedef struct { int parent; float off[3]; float seg[3]; float r; int axis; float amp, freq, phase; } BoneDef;
static BoneDef g_bones[NB];
static float g_bindpos[NB][3];
static ScePspFMatrix4 g_invbind_s4[NB];
static ScePspFMatrix4 g_proj_persp;

typedef struct { unsigned char w[8]; unsigned short u, v; signed char nx, ny, nz, pad; short x, y, z; } VSkin8;  /* 22 B */
typedef struct { unsigned char w[4]; unsigned short u, v; signed char nx, ny, nz, pad; short x, y, z; } VSkin4;  /* 18 B */
typedef struct { unsigned short u, v; signed char nx, ny, nz, pad; short x, y, z; } VRigid;                      /* 14 B */

#define RINGS 5
#define RV 8
#define VPB (RINGS * RV)            /* 40 vertices per bone */
#define TPB ((RINGS - 1) * RV * 2)  /* 64 triangles per bone -> 2048 per character */

static VSkin8 *g_v8;     /* NB*VPB, chunk k (8 bones) starts at k*8*VPB */
static VSkin4 *g_v4;
static VRigid *g_vr;
static unsigned short *g_bidx;   /* per-bone local indices (0..VPB-1), TPB*3 */
static unsigned short *g_cidx8;  /* chunk-local indices for an 8-bone chunk, 8*TPB*3 */
static unsigned short *g_cidx4;

static void bone(int i, int parent, float ox, float oy, float oz, float sx, float sy, float sz, float r, int axis, float amp)
{
    BoneDef *b = &g_bones[i];
    b->parent = parent; b->off[0] = ox; b->off[1] = oy; b->off[2] = oz;
    b->seg[0] = sx; b->seg[1] = sy; b->seg[2] = sz; b->r = r; b->axis = axis; b->amp = amp;
    b->freq = 2.0f + (i % 5) * 0.7f; b->phase = i * 0.37f;
}

static void skel_make(void)
{
    bone(0, -1, 0, 1.0f, 0, 0, 0.15f, 0, 0.14f, 1, 0.2f);
    bone(1, 0, 0, 0.15f, 0, 0, 0.15f, 0, 0.13f, 0, 0.1f);
    bone(2, 1, 0, 0.15f, 0, 0, 0.15f, 0, 0.14f, 0, 0.1f);
    bone(3, 2, 0, 0.15f, 0, 0, 0.15f, 0, 0.16f, 1, 0.1f);
    bone(4, 3, 0, 0.15f, 0, 0, 0.10f, 0, 0.06f, 0, 0.2f);
    bone(5, 4, 0, 0.10f, 0, 0, 0.22f, 0, 0.11f, 1, 0.3f);
    for (int s = 0; s < 2; s++) {
        float m = s ? -1.0f : 1.0f; int o = 6 + s * 4;
        bone(o + 0, 3, 0.05f * m, 0.12f, 0, 0.15f * m, 0, 0, 0.06f, 2, 0.1f);
        bone(o + 1, o + 0, 0.15f * m, 0, 0, 0.28f * m, 0, 0, 0.055f, 2, 0.6f);
        bone(o + 2, o + 1, 0.28f * m, 0, 0, 0.25f * m, 0, 0, 0.045f, 1, 0.8f);
        bone(o + 3, o + 2, 0.25f * m, 0, 0, 0.10f * m, 0, 0, 0.04f, 0, 0.4f);
        int l = 14 + s * 4;
        bone(l + 0, 0, 0.1f * m, 0, 0, 0, -0.45f, 0, 0.08f, 0, 0.6f);
        bone(l + 1, l + 0, 0, -0.45f, 0, 0, -0.45f, 0, 0.06f, 0, 0.7f);
        bone(l + 2, l + 1, 0, -0.45f, 0, 0, -0.05f, 0.15f, 0.05f, 0, 0.3f);
        bone(l + 3, l + 2, 0, -0.05f, 0.15f, 0, 0, 0.08f, 0.04f, 0, 0.2f);
    }
    for (int i = 0; i < 5; i++)       /* cape chain */
        bone(22 + i, i ? 21 + i : 3, 0, i ? -0.15f : 0.1f, i ? 0 : -0.12f, 0, -0.15f, 0, 0.12f, 0, 0.25f);
    for (int i = 0; i < 5; i++)       /* sword-ish chain on the right hand */
        bone(27 + i, i ? 26 + i : 13, i ? 0.2f : -0.1f, 0, 0, -0.2f, 0, 0, 0.03f, 1, 0.1f);

    for (int i = 0; i < NB; i++) {
        BoneDef *b = &g_bones[i];
        for (int k = 0; k < 3; k++) g_bindpos[i][k] = b->off[k] + (b->parent >= 0 ? g_bindpos[b->parent][k] : 0.0f);
        /* skin = world * T(-bind) * S(4): vertices are s16 normalised (/32768), stored as metres*8192 */
        gumLoadIdentity(&g_invbind_s4[i]);
        ScePspFVector3 t = {-g_bindpos[i][0], -g_bindpos[i][1], -g_bindpos[i][2]};
        gumTranslate(&g_invbind_s4[i], &t);
        ScePspFVector3 s = {4, 4, 4};
        gumScale(&g_invbind_s4[i], &s);
    }
}

static void skin_mesh_make(void)
{
    g_v8 = memalign(16, NB * VPB * sizeof(VSkin8));
    g_v4 = memalign(16, NB * VPB * sizeof(VSkin4));
    g_vr = memalign(16, NB * VPB * sizeof(VRigid));
    g_bidx = memalign(16, TPB * 3 * 2);
    g_cidx8 = memalign(16, 8 * TPB * 3 * 2);
    g_cidx4 = memalign(16, 4 * TPB * 3 * 2);
    int k = 0;
    for (int r = 0; r < RINGS - 1; r++)
        for (int i = 0; i < RV; i++) {
            unsigned short a = r * RV + i, b = r * RV + (i + 1) % RV, c = a + RV, d = b + RV;
            g_bidx[k++] = a; g_bidx[k++] = c; g_bidx[k++] = b;
            g_bidx[k++] = b; g_bidx[k++] = c; g_bidx[k++] = d;
        }
    for (int j = 0; j < 8; j++) for (int i = 0; i < TPB * 3; i++) g_cidx8[j * TPB * 3 + i] = g_bidx[i] + j * VPB;
    for (int j = 0; j < 4; j++) for (int i = 0; i < TPB * 3; i++) g_cidx4[j * TPB * 3 + i] = g_bidx[i] + j * VPB;

    for (int bi = 0; bi < NB; bi++) {
        BoneDef *b = &g_bones[bi];
        float d[3] = {b->seg[0], b->seg[1], b->seg[2]};
        float len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        float n[3] = {d[0] / len, d[1] / len, d[2] / len};
        float up[3] = {0, 1, 0};
        if (fabsf(n[1]) > 0.9f) { up[0] = 1; up[1] = 0; }
        float ax[3] = {up[1] * n[2] - up[2] * n[1], up[2] * n[0] - up[0] * n[2], up[0] * n[1] - up[1] * n[0]};
        float al = sqrtf(ax[0] * ax[0] + ax[1] * ax[1] + ax[2] * ax[2]);
        for (int q = 0; q < 3; q++) ax[q] /= al;
        float ay[3] = {n[1] * ax[2] - n[2] * ax[1], n[2] * ax[0] - n[0] * ax[2], n[0] * ax[1] - n[1] * ax[0]};
        for (int r = 0; r < RINGS; r++)
            for (int i = 0; i < RV; i++) {
                float t = (float)r / (RINGS - 1), ang = i * 2.0f * GU_PI / RV;
                float rad = b->r * (r == 0 || r == RINGS - 1 ? 0.8f : 1.0f);
                float nn[3], p[3];
                for (int q = 0; q < 3; q++) {
                    nn[q] = ax[q] * cosf(ang) + ay[q] * sinf(ang);
                    p[q] = g_bindpos[bi][q] + d[q] * t + nn[q] * rad;
                }
                int vi = bi * VPB + r * RV + i;
                int slot8 = bi % 8, slot4 = bi % 4;
                int par = b->parent;
                VSkin8 *v8 = &g_v8[vi];
                memset(v8, 0, sizeof(*v8));
                if (r == 0 && par >= 0 && par / 8 == bi / 8) { v8->w[slot8] = 64; v8->w[par % 8] = 64; }
                else v8->w[slot8] = 128;
                v8->u = (unsigned short)(i * 4096); v8->v = (unsigned short)(r * 4096 + (bi & 3) * 8192);
                v8->nx = (signed char)(nn[0] * 127); v8->ny = (signed char)(nn[1] * 127); v8->nz = (signed char)(nn[2] * 127);
                v8->x = (short)(p[0] * 8192); v8->y = (short)(p[1] * 8192); v8->z = (short)(p[2] * 8192);
                VSkin4 *v4 = &g_v4[vi];
                memset(v4, 0, sizeof(*v4));
                if (r == 0 && par >= 0 && par / 4 == bi / 4) { v4->w[slot4] = 64; v4->w[par % 4] = 64; }
                else v4->w[slot4] = 128;
                v4->u = v8->u; v4->v = v8->v; v4->nx = v8->nx; v4->ny = v8->ny; v4->nz = v8->nz;
                v4->x = v8->x; v4->y = v8->y; v4->z = v8->z;
                VRigid *vr = &g_vr[vi];
                vr->u = v8->u; vr->v = v8->v; vr->nx = v8->nx; vr->ny = v8->ny; vr->nz = v8->nz; vr->pad = 0;
                vr->x = v8->x; vr->y = v8->y; vr->z = v8->z;
            }
    }
    sceKernelDcacheWritebackAll();
}

/* CPU: 32 bone matrices (VFPU gum) for one character at time t */
static void anim_char(float t, int c, ScePspFMatrix4 *skin)
{
    static ScePspFMatrix4 world[NB];
    for (int i = 0; i < NB; i++) {
        BoneDef *b = &g_bones[i];
        ScePspFMatrix4 local;
        gumLoadIdentity(&local);
        ScePspFVector3 o = {b->off[0], b->off[1], b->off[2]};
        gumTranslate(&local, &o);
        float a = b->amp * sinf(b->freq * t + b->phase + c * 1.3f);
        if (b->axis == 0) gumRotateX(&local, a); else if (b->axis == 1) gumRotateY(&local, a); else gumRotateZ(&local, a);
        if (b->parent < 0) world[i] = local;
        else gumMultMatrix(&world[i], &world[b->parent], &local);
        gumMultMatrix(&skin[i], &world[i], &g_invbind_s4[i]);
    }
}

enum { SK_A8, SK_A4, SK_C };
static const char *sk_name[] = {"A8", "A4", "C"};
static ScePspFMatrix4 g_skin[8][NB];
static ScePspFMatrix4 g_charworld[8];

static void skin_state(void)
{
    sceGuSetMatrix(GU_PROJECTION, &g_proj_persp);
    sceGuSetMatrix(GU_VIEW, &g_ident);
    sceGuEnable(GU_DEPTH_TEST); sceGuDepthFunc(GU_GEQUAL); sceGuDepthMask(GU_FALSE);
    sceGuEnable(GU_CULL_FACE); sceGuFrontFace(GU_CW);
    sceGuDisable(GU_BLEND);
    sceGuEnable(GU_LIGHTING); sceGuEnable(GU_LIGHT0);
    ScePspFVector3 dir = {0.4f, 0.8f, 0.45f};
    sceGuLight(0, GU_DIRECTIONAL, GU_DIFFUSE, &dir);
    sceGuLightColor(0, GU_DIFFUSE, 0xffd0d0d0);
    sceGuAmbient(0xff404040);
    sceGuColor(0xffffffff);
    bind_tex(TX_CLUT8);
    sceGuTexScale(1.0f, 1.0f);
}

static void draw_chars(int nchars, int mode)
{
    for (int c = 0; c < nchars; c++) {
        if (mode == SK_C) {
            for (int b = 0; b < NB; b++) {
                ScePspFMatrix4 m;
                gumMultMatrix(&m, &g_charworld[c], &g_skin[c][b]);
                sceGuSetMatrix(GU_MODEL, &m);
                sceGuDrawArray(GU_TRIANGLES, GU_TEXTURE_16BIT | GU_NORMAL_8BIT | GU_VERTEX_16BIT | GU_INDEX_16BIT | GU_TRANSFORM_3D,
                               TPB * 3, g_bidx, g_vr + b * VPB);
            }
            continue;
        }
        sceGuSetMatrix(GU_MODEL, &g_charworld[c]);
        int csz = mode == SK_A8 ? 8 : 4;
        for (int ch = 0; ch < NB / csz; ch++) {
            for (int j = 0; j < csz; j++) sceGuBoneMatrix(j, &g_skin[c][ch * csz + j]);
            if (mode == SK_A8)
                sceGuDrawArray(GU_TRIANGLES, GU_WEIGHTS(8) | GU_WEIGHT_8BIT | GU_TEXTURE_16BIT | GU_NORMAL_8BIT | GU_VERTEX_16BIT |
                               GU_INDEX_16BIT | GU_TRANSFORM_3D, 8 * TPB * 3, g_cidx8, g_v8 + ch * 8 * VPB);
            else
                sceGuDrawArray(GU_TRIANGLES, GU_WEIGHTS(4) | GU_WEIGHT_8BIT | GU_TEXTURE_16BIT | GU_NORMAL_8BIT | GU_VERTEX_16BIT |
                               GU_INDEX_16BIT | GU_TRANSFORM_3D, 4 * TPB * 3, g_cidx4, g_v4 + ch * 4 * VPB);
        }
    }
}

static void place_chars(int nchars)
{
    for (int c = 0; c < nchars; c++) {
        gumLoadIdentity(&g_charworld[c]);
        float span = nchars > 1 ? 7.0f : 0.0f;
        ScePspFVector3 p = {nchars > 1 ? -span / 2 + span * c / (nchars - 1) : 0.0f, -1.0f, -5.0f - (c & 1) * 0.6f};
        gumTranslate(&g_charworld[c], &p);
        gumRotateY(&g_charworld[c], 0.6f * c - 1.5f);
    }
}

typedef struct { float avg, p95, max, cpu, build, wait; int frames; } LoopRes;

static int cmpf(const void *a, const void *b) { float x = *(const float *)a, y = *(const float *)b; return x < y ? -1 : x > y; }

static float g_ft[300];

static void skin_loop(int nchars, int mode, int world, int frames, LoopRes *r)
{
    place_chars(nchars);
    memset(r, 0, sizeof(*r));
    if (frames > 300) frames = 300;
    for (int f = 0; f < frames; f++) {
        u64 t0 = now_us();
        float t = f / 30.0f;
        for (int c = 0; c < nchars; c++) anim_char(t, c, g_skin[c]);
        sceKernelDcacheWritebackAll();       /* matrices are copied into the list by sceGuBoneMatrix; list itself must reach memory */
        u64 t1 = now_us();
        sceGuStart(GU_DIRECT, g_list);
        sceGuClearColor(0xff403028);
        sceGuClearDepth(0);
        sceGuClear(GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT);
        if (world) sceGuCallList(g_call_world);
        skin_state();
        draw_chars(nchars, mode);
        sceGuFinish();
        u64 t2 = now_us();
        if (diag_ge_wait("bench skin frame", 3000, 1)) blog("GE HANG in skin frame %d (GE reset)", f);
        diag_beat();
        u64 t3 = now_us();
        g_ft[f] = (t3 - t0) / 1000.0f;
        r->cpu += (t1 - t0) / 1000.0f; r->build += (t2 - t1) / 1000.0f; r->wait += (t3 - t2) / 1000.0f;
        r->avg += g_ft[f];
        if (f % 10 == 9 || f == frames - 1) {
            char s[64];
            snprintf(s, sizeof(s), "skin %d x %s%s %d/%d", nchars, sk_name[mode], world ? "+world" : "", f + 1, frames);
            present(s, 0);
        } else {
            g_draw = sceGuSwapBuffers();
        }
    }
    r->frames = frames;
    r->avg /= frames; r->cpu /= frames; r->build /= frames; r->wait /= frames;
    qsort(g_ft, frames, sizeof(float), cmpf);
    r->p95 = g_ft[(frames * 95) / 100 < frames ? (frames * 95) / 100 : frames - 1];
    r->max = g_ft[frames - 1];
}

/* ------------------------------------------------------------------ sections */
static int g_free_game_kb, g_maxfree_kb, g_totfree_kb, g_static_kb;
static float g_ms_read = -1, g_ms_write = -1;

static void sec_sys(void)
{
    int model = kuKernelGetModel();
    int devkit = sceKernelDevkitVersion();
    g_maxfree_kb = (int)(sceKernelMaxFreeMemSize() / 1024);
    g_totfree_kb = (int)(sceKernelTotalFreeMemSize() / 1024);
    g_static_kb = (int)((sizeof(g_list) + sizeof(g_call) + sizeof(g_call_world) + sizeof(g_log) +
                         sizeof(g_tex4) + sizeof(g_tex8) + sizeof(g_tex16)) / 1024);
    g_free_game_kb = g_maxfree_kb + HEAP_KB + g_static_kb;
    blog("== sys ==");
    blog("boot=%s", g_boot);
    if (model >= 0) blog("model=%d (generation %02dg; 0=1000 1=2000 2,3,6,8=3000 4=Go 10=E1000)", model, model + 1);
    else blog("model=unknown (kuKernelGetModel=0x%08X, no KUBridge)", (unsigned)model);
    blog("devkit=0x%08X (firmware %d.%d%d)", (unsigned)devkit, (devkit >> 24) & 0xf, (devkit >> 16) & 0xf, (devkit >> 8) & 0xf);
    blog("mem.maxfree_kb=%d  mem.totalfree_kb=%d  (after display+GU init)", g_maxfree_kb, g_totfree_kb);
    blog("mem.bench_heap_kb=%d  mem.bench_static_kb~%d", HEAP_KB, g_static_kb);
    blog("mem.free_for_game_kb=%d (maxfree+bench heap+statics)", g_free_game_kb);
    blog("mem.mode=%s", g_free_game_kb > 30 * 1024 ? "EXTENDED (64MB mode)" : "32MB mode (~24MB user)");
    blog("emulator_guess(boot path)=%s", g_emu_guess ? "YES (not ms0:/ef0:)" : "no (MS speed checked later)");
    blog("build=" ASH_GIT_SHA " " __DATE__ " " __TIME__ " v" BENCH_VERSION);
}

static void sec_cpu(void)
{
    CpuRes r222, r333;
    blog("== cpu ==");
    scePowerSetClockFrequency(222, 222, 111);
    sceKernelDelayThread(100000);
    cpu_tests(&r222);
    scePowerSetClockFrequency(333, 333, 166);
    sceKernelDelayThread(100000);
    cpu_tests(&r333);
    CpuRes *rs[2] = {&r222, &r333};
    for (int i = 0; i < 2; i++)
        blog("cpu@%d/%d: matmul fpu=%.0fns vfpu=%.0fns pose32=%.0fus loop=%.1fms",
             rs[i]->cpu, rs[i]->bus, rs[i]->mm_fpu_ns, rs[i]->mm_vfpu_ns, rs[i]->pose_us, rs[i]->loop_ms);
    blog("cpu.speedup_333_vs_222=%.2fx (expect ~1.50)", r222.loop_ms / r333.loop_ms);
    blog("cpu.anim_6chars_32bones_ms~%.2f (pose sampling, FPU, @333)", r333.pose_us * 6 / 1000.0f);
    suml(C_WHITE, "CPU %d MHz x%.2f", r333.cpu, r222.loop_ms / r333.loop_ms);
}

static void sec_ms(void)
{
    blog("== ms (memory stick) ==");
    const int CH = 256 * 1024, TOTAL = 8 * 1024 * 1024;
    char path[300];
    snprintf(path, sizeof(path), "%s/speed.tmp", g_dir);
    unsigned char *buf = memalign(64, CH);
    if (!buf) { blog("ms: no memory"); return; }
    for (int i = 0; i < CH; i++) buf[i] = (unsigned char)(i * 31);
    present("MS write 8 MB...", 1);
    SceUID fd = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0) { blog("ms: cannot create %s (0x%08X)", path, (unsigned)fd); free(buf); return; }
    u64 t0 = now_us();
    int done = 0;
    while (done < TOTAL) { int w = sceIoWrite(fd, buf, CH); if (w <= 0) break; done += w; }
    sceIoClose(fd);
    u64 t1 = now_us();
    g_ms_write = done / 1048576.0f / ((t1 - t0) / 1e6f);
    blog("ms.write_8mb=%.2f MB/s (%d KB in %.0f ms)", g_ms_write, done / 1024, (t1 - t0) / 1000.0f);
    int chunks[2] = {CH, 32 * 1024};
    for (int k = 0; k < 2; k++) {
        present(k ? "MS read 8 MB (32K)..." : "MS read 8 MB (256K)...", 1);
        fd = sceIoOpen(path, PSP_O_RDONLY, 0);
        if (fd < 0) { blog("ms: cannot open for read"); break; }
        t0 = now_us();
        int got = 0;
        while (got < TOTAL) { int r = sceIoRead(fd, buf, chunks[k]); if (r <= 0) break; got += r; }
        sceIoClose(fd);
        t1 = now_us();
        float mbs = got / 1048576.0f / ((t1 - t0) / 1e6f);
        blog("ms.read_8mb_chunk%dk=%.2f MB/s (%.0f ms)", chunks[k] / 1024, mbs, (t1 - t0) / 1000.0f);
        if (k == 0) g_ms_read = mbs;
    }
    sceIoRemove(path);
    free(buf);
    if (g_ms_read > 40.0f) { g_emu_guess = 1; blog("emulator_guess=YES (MS read > 40 MB/s is not a real stick)"); }
    suml(C_WHITE, "MS read %.1f MB/s", g_ms_read);
}

static Grid g_tiny, g_open, g_full;

static void sec_tri(void)
{
    blog("== tri: GE ms, tiny triangles (vertex-bound), 333 MHz ==");
    grid_make(&g_tiny, 100, 150, 120, 68, 240, 136, 1, 1);
    struct { int tx, prim, z; } T[] = {
        {TX_NONE, P_IDX, 1}, {TX_CLUT4, P_IDX, 1}, {TX_CLUT8, P_IDX, 1}, {TX_CLUT8_VRAM, P_IDX, 1}, {TX_565, P_IDX, 1},
        {TX_CLUT4, P_IDX, 0}, {TX_CLUT4, P_FLAT, 1}, {TX_CLUT4, P_STRIP, 1}, {TX_CLUT4, P_FLOAT, 1},
    };
    for (unsigned i = 0; i < sizeof(T) / sizeof(T[0]); i++) {
        const int n = 10000;
        sceGuStart(GU_CALL, g_call);
        common_state(T[i].z);
        bind_tex(T[i].tx);
        grid_draw(&g_tiny, T[i].tx, T[i].prim, n);
        sceGuFinish();
        sceKernelDcacheWritebackAll();
        GeTime g = ge_time_call(g_call, 6, 1);
        blog("tri.%s.%s.%s.10k: %.2f ms (min %.2f) = %.3f ms/1k", tx_name[T[i].tx], p_name[T[i].prim], T[i].z ? "z" : "noz",
             g.avg, g.min, g.avg / 10.0f);
        char s[64];
        snprintf(s, sizeof(s), "tri %s %s %s", tx_name[T[i].tx], p_name[T[i].prim], T[i].z ? "z" : "noz");
        present(s, 1);
    }
    int counts[] = {5000, 10000, 20000, 30000};
    for (int i = 0; i < 4; i++) {
        sceGuStart(GU_CALL, g_call);
        common_state(1);
        bind_tex(TX_CLUT4);
        grid_draw(&g_tiny, TX_CLUT4, P_IDX, counts[i]);
        sceGuFinish();
        sceKernelDcacheWritebackAll();
        GeTime g = ge_time_call(g_call, 6, 1);
        blog("tri.scale.clut4.idx.%dk: %.2f ms = %.3f ms/1k", counts[i] / 1000, g.avg, g.avg * 1000.0f / counts[i]);
        present("tri scaling", 1);
    }
}

static void sec_fill(void)
{
    blog("== fill: full-screen layers (480x272 each) ==");
    grid_make(&g_full, 8, 1, 0, 0, 480, 272, 0, 0);
    const char *kind[] = {"flat", "clut4", "clut4+blend"};
    for (int k = 0; k < 3; k++)
        for (int layers = 1; layers <= 4; layers *= 2) {
            sceGuStart(GU_CALL, g_call);
            common_state(1);
            bind_tex(k == 0 ? TX_NONE : TX_CLUT4);
            if (k == 2) {
                sceGuEnable(GU_BLEND);
                sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
                sceGuColor(0x80ffffff);
            } else sceGuColor(0xffc0c0c0);
            for (int l = 0; l < layers; l++) grid_draw(&g_full, k == 0 ? TX_NONE : TX_CLUT4, P_IDX, 16);
            sceGuFinish();
            sceKernelDcacheWritebackAll();
            GeTime g = ge_time_call(g_call, 6, 1);
            float mpix = layers * 480.0f * 272.0f / 1e6f / (g.avg / 1000.0f);
            blog("fill.%s.x%d: %.2f ms (%.0f Mpix/s)", kind[k], layers, g.avg, mpix);
            char s[64];
            snprintf(s, sizeof(s), "fill %s x%d", kind[k], layers);
            present(s, 1);
        }
}

static float g_gate2 = -1;

static void build_open(unsigned int *buf, int lightmap)
{
    sceGuStart(GU_CALL, buf);
    common_state(1);
    bind_tex(TX_CLUT4);
    sceGuColor(0xffffffff);
    grid_draw(&g_open, TX_CLUT4, P_IDX, 5000);
    grid_draw(&g_open, TX_CLUT4, P_IDX, 5000);
    if (lightmap) {
        sceGuDepthFunc(GU_EQUAL);
        sceGuEnable(GU_BLEND);
        sceGuBlendFunc(GU_ADD, GU_DST_COLOR, GU_FIX, 0, 0);
        grid_draw(&g_open, TX_CLUT4, P_IDX, 5000);
        grid_draw(&g_open, TX_CLUT4, P_IDX, 5000);
        sceGuDisable(GU_BLEND);
        sceGuDepthFunc(GU_GEQUAL);
    }
    sceGuFinish();
    sceKernelDcacheWritebackAll();
}

static void sec_open(void)
{
    blog("== open view: 10k tris CLUT4, 2 layers = overdraw 2x ==");
    grid_make(&g_open, 50, 50, 0, 0, 480, 272, 0, 0);
    build_open(g_call, 0);
    GeTime g = ge_time_call(g_call, 8, 1);
    g_gate2 = g.avg;
    blog("open.10k.clut4.od2: %.2f ms (min %.2f)", g.avg, g.min);
    present("open view 10k", 1);
    build_open(g_call, 1);
    g = ge_time_call(g_call, 8, 1);
    blog("open.10k.+lightmap_pass (20k tris drawn): %.2f ms", g.avg);
    present("open view + lightmap pass", 1);
    build_open(g_call_world, 0);    /* kept for skin+world */
}

static LoopRes g_gate1;

static void sec_skin(void)
{
    blog("== skin: chars x 2048 tris x 32 bones; frame ms (no vsync) ==");
    blog("   cpu=bone matrices (VFPU gum), build=list, wait=GE after list end");
    skel_make();
    skin_mesh_make();
    struct { int n, mode, world, frames; } S[] = {
        {1, SK_A8, 0, 120}, {4, SK_A8, 0, 120}, {6, SK_A8, 0, 300}, {8, SK_A8, 0, 120},
        {6, SK_A4, 0, 120}, {6, SK_C, 0, 120}, {6, SK_A8, 1, 120},
    };
    for (unsigned i = 0; i < sizeof(S) / sizeof(S[0]); i++) {
        LoopRes r;
#ifdef ASH_BENCH_HOLD   /* debug build: keep rendering scene #ASH_BENCH_HOLD for screenshots */
        if ((int)i == ASH_BENCH_HOLD) for (;;) skin_loop(S[i].n, S[i].mode, S[i].world, 30, &r);
#endif
        skin_loop(S[i].n, S[i].mode, S[i].world, S[i].frames, &r);
        blog("skin.%dx%s%s: avg %.2f ms (%.1f fps) p95 %.2f max %.2f | cpu %.2f build %.2f wait %.2f",
             S[i].n, sk_name[S[i].mode], S[i].world ? "+world10k" : "", r.avg, 1000.0f / r.avg, r.p95, r.max, r.cpu, r.build, r.wait);
        if (S[i].n == 6 && S[i].mode == SK_A8 && !S[i].world) g_gate1 = r;
    }
}

static int g_gate3_margin_kb = -999999;
static int g_gate3_scene_kb;
static int g_gate3_real;

static void sec_gate3(void)
{
    blog("== gate3: cooked scene vs free RAM ==");
    char parent[256], path[300];
    snprintf(parent, sizeof(parent), "%s", g_dir);
    char *sl = strrchr(parent, '/');
    if (sl) *sl = 0;
    char p1[300], p2[300];
    snprintf(p1, sizeof(p1), "%s/ASHVIEW/scene.ashm", parent);
    snprintf(p2, sizeof(p2), "%s/scene.ashm", g_dir);
    const char *cands[2] = {p1, p2};
    int size = -1;
    path[0] = 0;
    for (int i = 0; i < 2 && size < 0; i++) {
        SceUID fd = sceIoOpen(cands[i], PSP_O_RDONLY, 0);
        if (fd < 0) continue;
        size = (int)sceIoLseek(fd, 0, PSP_SEEK_END);
        sceIoClose(fd);
        snprintf(path, sizeof(path), "%s", cands[i]);
    }
    g_gate3_real = size > 0;
    if (!g_gate3_real) size = SYNTH_SCENE;
    g_gate3_scene_kb = (size + 1023) / 1024;
    blog("gate3.scene=%s (%d KB)", g_gate3_real ? path : "SYNTHETIC (no ASHVIEW/scene.ashm)", g_gate3_scene_kb);

    SceUID blk = sceKernelAllocPartitionMemory(2, "ashscene", PSP_SMEM_Low, size, NULL);
    if (blk < 0) blog("gate3.alloc=FAILED 0x%08X", (unsigned)blk);
    else {
        void *mem = sceKernelGetBlockHeadAddr(blk);
        u64 t0 = now_us();
        if (g_gate3_real) {
            SceUID fd = sceIoOpen(path, PSP_O_RDONLY, 0);
            int got = 0;
            while (fd >= 0 && got < size) {
                int r = sceIoRead(fd, (char *)mem + got, size - got > 262144 ? 262144 : size - got);
                if (r <= 0) break;
                got += r;
            }
            if (fd >= 0) sceIoClose(fd);
            u64 t1 = now_us();
            float mbs = got / 1048576.0f / ((t1 - t0) / 1e6f);
            blog("gate3.load=%d KB in %.0f ms (%.2f MB/s)", got / 1024, (t1 - t0) / 1000.0f, mbs);
            if (mbs > 40.0f && !g_emu_guess) { g_emu_guess = 1; blog("emulator_guess=YES (scene read > 40 MB/s)"); }
        } else {
            memset(mem, 0x5a, size);
        }
        blog("gate3.alloc=OK, maxfree after scene=%d KB", (int)(sceKernelMaxFreeMemSize() / 1024));
        sceKernelFreePartitionMemory(blk);
    }
    g_gate3_margin_kb = g_free_game_kb - PERM_KB - RESERVE_KB - g_gate3_scene_kb;
    blog("gate3.margin_kb=%d = free %d - perm %d - reserve %d - scene %d", g_gate3_margin_kb, g_free_game_kb, PERM_KB,
         RESERVE_KB, g_gate3_scene_kb);
}

static const char *grade(int g) { return g == 0 ? "GREEN" : g == 1 ? "YELLOW" : "RED"; }
static unsigned short gcol(int g) { return g == 0 ? C_GREEN : g == 1 ? C_YELLOW : C_RED; }

static void sec_gates(void)
{
    char line[256];
    int n = 0;
    blog("== gates (PLAN 7.1) ==");
#ifndef ASH_MEM_ONLY
    float fps = g_gate1.avg > 0 ? 1000.0f / g_gate1.avg : 0;
    int g1 = (fps >= 30.0f && g_gate1.p95 <= 34.0f) ? 0 : fps >= 25.0f ? 1 : 2;
    int g2 = g_gate2 <= 12.0f ? 0 : g_gate2 <= 20.0f ? 1 : 2;
    n += snprintf(line + n, sizeof(line) - n, "GATE1=%.1ffps(p95 %.1fms) %s | GATE2=%.2fms %s | ", fps, g_gate1.p95, grade(g1),
                  g_gate2, grade(g2));
    suml(gcol(g1), "G1 6chars %.1ffps %s", fps, grade(g1));
    suml(gcol(g2), "G2 open10k %.1fms %s", g_gate2, grade(g2));
#endif
    int g3 = g_gate3_margin_kb >= 1024 ? 0 : g_gate3_margin_kb >= 0 ? 1 : 2;
    n += snprintf(line + n, sizeof(line) - n, "GATE3=%+.1fMB %s (%s, scene %s %dKB)%s", g_gate3_margin_kb / 1024.0f, grade(g3),
                  g_free_game_kb > 30 * 1024 ? "64MB mode" : "32MB mode", g_gate3_real ? "real" : "synthetic", g_gate3_scene_kb,
                  g_emu_guess ? " [EMULATOR?]" : "");
    blog("%s", line);
    suml(gcol(g3), "G3 RAM %+.1fMB %s", g_gate3_margin_kb / 1024.0f, grade(g3));
    suml(C_WHITE, "Free %d KB (%s)", g_free_game_kb, g_free_game_kb > 30 * 1024 ? "64MB" : "32MB");
}

/* ------------------------------------------------------------------ results screen */
static void results_screen(void)
{
    int nlines = 0;
    static const char *lines[1024];
    static int lens[1024];
    for (int i = 0; i < g_loglen && nlines < 1024;) {
        int j = i;
        while (j < g_loglen && g_log[j] != '\n') j++;
        lines[nlines] = g_log + i; lens[nlines++] = j - i;
        i = j + 1;
    }
    const int per = 30;
    int pages = 1 + (nlines + per - 1) / per, page = 0;
    unsigned int prev = 0;
    for (;;) {
        gu_clear(0xff100808);
        unsigned short *fb = fb_ptr(g_draw);
        char hdr[80];
        if (page == 0) {
#ifdef ASH_MEM_ONLY
            text(fb, 8, 6, 2, C_CYAN, "ASH MEM64 DONE");
#else
            text(fb, 8, 6, 2, C_CYAN, "ASH BENCH DONE");
#endif
            if (g_emu_guess) text(fb, 8 + 15 * 16, 6, 2, C_RED, "EMULATOR?");
            for (int i = 0; i < g_nsum; i++) text(fb, 8, 30 + i * 18, 2, g_sumcol[i], g_sum[i]);
            text(fb, 8, SCR_H - 34, 1, C_WHITE, "Saved:");
            text(fb, 8 + 7 * 8, SCR_H - 34, 1, strncmp(g_saved, "FAILED", 6) ? C_GREEN : C_RED, g_saved);
        } else {
            int first = (page - 1) * per;
            for (int i = 0; i < per && first + i < nlines; i++) {
                char buf[64];
                int l = lens[first + i] < 59 ? lens[first + i] : 59;
                memcpy(buf, lines[first + i], l); buf[l] = 0;
                text(fb, 4, 4 + i * 8, 1, buf[0] == '=' ? C_CYAN : (strncmp(buf, "GATE", 4) ? C_WHITE : C_YELLOW), buf);
            }
        }
        snprintf(hdr, sizeof(hdr), "page %d/%d  LEFT/RIGHT or L/R = page  START = exit", page + 1, pages);
        text(fb, 8, SCR_H - 14, 1, C_GREY, hdr);
        sceDisplayWaitVblankStart();
        g_draw = sceGuSwapBuffers();
        scePowerTick(PSP_POWER_TICK_DISPLAY);
        SceCtrlData pad;
        sceCtrlPeekBufferPositive(&pad, 1);
        unsigned int pressed = pad.Buttons & ~prev;
        prev = pad.Buttons;
        if (pressed & (PSP_CTRL_RIGHT | PSP_CTRL_RTRIGGER | PSP_CTRL_CROSS)) page = (page + 1) % pages;
        if (pressed & (PSP_CTRL_LEFT | PSP_CTRL_LTRIGGER)) page = (page + pages - 1) % pages;
        if (pressed & PSP_CTRL_START) break;
    }
}

/* ------------------------------------------------------------------ main */
int main(int argc, char **argv)
{
    int th = sceKernelCreateThread("cb_thread", cb_thread, 0x11, 0xFA0, 0, 0);
    if (th >= 0) sceKernelStartThread(th, 0, 0);
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_DIGITAL);

    if (argc > 0 && argv[0]) {
        snprintf(g_boot, sizeof(g_boot), "%s", argv[0]);
        char *sl = strrchr(g_boot, '/');
        if (sl) { int l = sl - g_boot; memcpy(g_dir, g_boot, l); g_dir[l] = 0; }
    }
    g_emu_guess = !(strncmp(g_boot, "ms0:", 4) == 0 || strncmp(g_boot, "ef0:", 4) == 0);
    diag_init(argc > 0 ? argv[0] : NULL, ASH_GIT_SHA);   /* log.txt / crash.txt / hang watchdog (src/diag) */

    scePowerSetClockFrequency(333, 333, 166);
    gu_init();
    gumLoadIdentity(&g_ident);
    gumLoadIdentity(&g_proj_ortho);
    gumOrtho(&g_proj_ortho, 0, 480, 272, 0, -1, 0.01f);   /* z=0 lands near the far plane: characters (perspective) draw over the world */
    gumLoadIdentity(&g_model_px16);
    { ScePspFVector3 s = {2048, 2048, 1}; gumScale(&g_model_px16, &s); }
    gumLoadIdentity(&g_proj_persp);
    gumPerspective(&g_proj_persp, 60.0f, 480.0f / 272.0f, 0.5f, 100.0f);

    blog("ASH BENCH v" BENCH_VERSION " (PLAN.md 6.6 / 7.1). Emulator numbers != hardware.");
    diag_stage_t("sys", 30000);
    sec_sys();
    save_results();
    gu_clear(0xff302020);
    present("Ash Bench: starting", 1);
#ifndef ASH_MEM_ONLY
    diag_stage_t("cpu", 30000);  sec_cpu();          save_results();
    diag_stage_t("ms", 60000);   sec_ms();           save_results();
    diag_stage("textures");      make_textures();
    diag_stage_t("tri", 8000);   sec_tri();          save_results();
    diag_stage_t("fill", 8000);  sec_fill();         save_results();
    diag_stage_t("open", 8000);  sec_open();         save_results();
    diag_stage_t("skin", 8000);  sec_skin();         save_results();
#endif
    diag_stage_t("gate3", 30000);
    sec_gate3();
    sec_gates();
    blog("== done ==");
    save_results();
    blog("saved: %s", g_saved);
    diag_stage_t("results screen (waits for buttons)", 0x7fffffff);
    results_screen();
    sceGuTerm();
    sceKernelExitGame();
    return 0;
}
