/*
 * Ash PSP-est: a cost model of a real PSP-3000 for runs in PPSSPP (docs/psp-cost-model.md).
 *
 * PPSSPP does not charge 3D fill, does not emulate caches and runs MIPS code at 1 cycle per instruction, so its
 * timings say nothing about the hardware. This module estimates, per frame, what a real PSP would spend:
 *   GE  - every finished display list (and the lists it CALLs) is walked command by command: vertices by format,
 *         skinning weights, lighting, triangles, texture/CLUT changes, and (coverage=1) covered pixels by
 *         transforming the vertices on the CPU (overdraw, minification, RAM/VRAM textures, blending, Z);
 *   CPU - emulated CPU time (sceKernelGetSystemTimeLow) x cpu_k;
 * and prints "PSPEST ..." lines + a 2-line overlay. Coefficients: src/perf/psp_cost.cfg (+ runtime override file).
 *
 * Two ways to plug it in:
 *  1. zero-touch: link with  -Wl,--wrap=sceGuStart,--wrap=sceGuFinish,--wrap=sceGuSync,--wrap=sceGuSwapBuffers,
 *     --wrap=sceGuDrawBuffer  (src/perf/CMakeLists.txt does that for ash_game_est / ash_viewer_est / ash_bench_est);
 *  2. a small patch in the game (docs/psp-cost-model.md "Integration"): the same wraps + perf_begin/perf_end around
 *     the CPU systems for a per-system breakdown, perf_count_* for things the GE list does not show.
 * Everything here costs CPU time; the model subtracts its own time from the CPU measurement.
 */
#ifndef ASH_PERF_H
#define ASH_PERF_H

#ifdef __cplusplus
extern "C" {
#endif

/* CPU sections (perf_begin/perf_end); times are emulated us x cpu_k */
enum {
    PERF_SIM = 0,   /* gameplay / FSM */
    PERF_AI,
    PERF_ANIM,
    PERF_COL,       /* collision, controllers, camera casts */
    PERF_CAM,
    PERF_LIST,      /* visibility + display list building */
    PERF_AUDIO,
    PERF_OTHER,
    PERF_NSEC
};

void perf_init(void);                        /* idempotent; also run automatically before main() */
void perf_begin(int sec);
void perf_end(int sec);
void perf_count(int what, int n);            /* PERF_C_* counters for the log */
enum { PERF_C_CHARS = 0, PERF_C_BONES, PERF_C_COLTESTS, PERF_C_AI_AGENTS, PERF_C_NCOUNT };
#define perf_count_chars(n)    perf_count(PERF_C_CHARS, (n))
#define perf_count_bones(n)    perf_count(PERF_C_BONES, (n))
#define perf_count_coltests(n) perf_count(PERF_C_COLTESTS, (n))
#define perf_count_ai(n)       perf_count(PERF_C_AI_AGENTS, (n))

/* Manual hooks (what the --wrap functions call); use them directly if the build can't use --wrap. */
void perf_list_started(int cid, const void *list);           /* after sceGuStart */
void perf_list_finished(int size_bytes);                      /* after sceGuFinish */
void perf_ge_synced(void);                                    /* after sceGuSync: walks the finished lists */
void perf_frame_end(void *draw_buffer_vram_rel);              /* before sceGuSwapBuffers: estimate, overlay, log */
void perf_set_draw_buffer(int psm, void *fbp_vram_rel, int fbw);

/* last frame's estimate (ms on a PSP-3000 at the clock the program runs / at 333 MHz) */
typedef struct {
    float cpu_ms, ge_ms, frame_ms, fps;       /* at the current clock */
    float cpu333_ms, ge333_ms, frame333_ms, fps333;
    float worst_frame333_ms;
    int tris, draws, verts, px;
} PerfFrame;
const PerfFrame *perf_last(void);

#ifdef __cplusplus
}
#endif
#endif
