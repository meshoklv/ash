/*
 * Ash PSP-est runtime: frame accounting, CPU timing, memory "hard mode", budgets, overlay, log (see perf.h).
 * Log lines (stdout -> PPSSPPHeadless run.log), parsed by tools/perf/report.py:
 *   PSPEST-INIT ...           config, clocks, memory at start
 *   PSPEST f=N k=v ...        one per frame (log_every)
 *   PSPEST-MEM ...            free-heap probes
 *   PSPEST-WARN kind=...      budget / hardware-limit violations (first 5 of each kind, then every 300th)
 */
#include "perf.h"
#include "ge_scan.h"

#include <malloc.h>
#include <math.h>
#include <pspiofilemgr.h>
#include <pspkernel.h>
#include <psppower.h>
#include <pspsysmem.h>
#include <pspthreadman.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

float g_pc[PC__COUNT];

static int g_inited;
static unsigned int g_self_us;          /* perf's own time inside the current frame's CPU segments */
static unsigned int g_seg_start;        /* start of the current active CPU segment */
static double g_cpu_us, g_pre_us;       /* active CPU this frame; part before the first DIRECT list */
static int g_list_seen;
static unsigned int g_frame_no;
static GeStats g_fs;                    /* GE stats of the current frame */
static PerfFrame g_last;
static double g_sec_us[PERF_NSEC];
static unsigned int g_sec_t0[PERF_NSEC];
static int g_counts[PERF_C_NCOUNT];
static int g_cpu_mhz = 222, g_bus_mhz = 111;
static void *g_draw;
static int g_psm = 0, g_fbw = 512;
static unsigned int g_swap_t, g_wait_us;   /* wall time between swaps; time inside the real sceGuSync */

#define MAXQ 16
static struct { const void *start; int size; } g_q[MAXQ];
static int g_nq;
static const void *g_cur_list;
static int g_cur_cid = -1;

/* worst / stats */
static float g_worst333;
static double g_sum333;
static unsigned int g_nframes;

/* memory */
static int g_mem_free_min_kb = 1 << 30, g_ballast_kb, g_heap_cap_kb, g_mode64;
static void *g_ballast;

static const char *const g_sec_names[PERF_NSEC] = {"sim", "ai", "anim", "col", "cam", "list", "audio", "other"};

static inline unsigned int now_us(void) { return sceKernelGetSystemTimeLow(); }

/* ------------------------------------------------------------------ config */
static void cfg_apply(char *txt)
{
    char *line = txt;
    while (line && *line) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        char *hash = strchr(line, '#');
        if (hash) *hash = 0;
        char key[64];
        float val;
        if (sscanf(line, " %63[a-z0-9_] = %f", key, &val) == 2)
            for (int i = 0; i < PC__COUNT; i++)
                if (strcmp(key, g_pc_names[i]) == 0) { g_pc[i] = val; break; }
        line = nl ? nl + 1 : NULL;
    }
}

static int cfg_load(char *found, int flen)
{
    static const char *paths[] = {"psp_cost.cfg", "host0:/psp_cost.cfg", "ms0:/ash/psp_cost.cfg", NULL};
    for (int i = 0; paths[i]; i++) {
        SceUID fd = sceIoOpen(paths[i], PSP_O_RDONLY, 0);
        if (fd < 0) continue;
        static char buf[16384];
        int n = sceIoRead(fd, buf, sizeof(buf) - 1);
        sceIoClose(fd);
        if (n <= 0) continue;
        buf[n] = 0;
        cfg_apply(buf);
        snprintf(found, flen, "%s", paths[i]);
        return 1;
    }
    snprintf(found, flen, "compiled defaults");
    return 0;
}

/* ------------------------------------------------------------------ memory */
static int probe_free_heap_kb(void)
{
    /* largest malloc-able block, binary search in 16 KB steps (cheap: newlib trims the top on free) */
    unsigned int lo = 0, hi = 64u * 1024 * 1024;
    while (hi - lo > 16 * 1024) {
        unsigned int mid = lo + (hi - lo) / 2;
        void *volatile p = malloc(mid);   /* volatile: GCC would fold malloc+free into "always succeeds" */
        if (p) { free(p); lo = mid; } else hi = mid;
    }
    return (int)(lo / 1024);
}

static void mem_init(void)
{
    unsigned int t0 = now_us();
    int maxfree = (int)(sceKernelMaxFreeMemSize() / 1024), total = (int)(sceKernelTotalFreeMemSize() / 1024);
    int heap_free = probe_free_heap_kb();
    struct mallinfo mi = mallinfo();
    int heap_used = (int)(mi.uordblks / 1024);
    g_heap_cap_kb = heap_free + heap_used;
    /* user memory the program can see now = partition free + its heap; > 30 MB means the 64 MB partition */
    int seen = maxfree + g_heap_cap_kb;
    g_mode64 = seen > 30 * 1024;
    int prof = (int)PC(mem_profile);
    g_ballast_kb = (int)PC(mem_hw_minus_emu_kb);
    if (prof == 32 && g_mode64) g_ballast_kb += (int)(PC(mem_user_kb_64) - PC(mem_user_kb_32));
    printf("PSPEST-INIT mem emu_partition=%s maxfree_kb=%d totalfree_kb=%d heap_free_kb=%d heap_used_kb=%d "
           "profile=%d hw_minus_emu_kb=%d ballast_kb=%d enforce=%d\n", g_mode64 ? "64MB" : "32MB(24MB user)", maxfree,
           total, heap_free, heap_used, prof, (int)PC(mem_hw_minus_emu_kb), g_ballast_kb, (int)PC(mem_enforce));
    if (prof == 64 && !g_mode64)
        printf("PSPEST-WARN kind=mem_profile value=32MB budget=64MB note=EBOOT runs in the 24 MB partition (MEMSIZE!=1)\n");
    if (PC(mem_enforce) > 0 && g_ballast_kb > 0) {
        g_ballast = malloc((size_t)g_ballast_kb * 1024);
        printf("PSPEST-INIT mem ballast %d KB %s -> malloc now fails where a real PSP (profile %d) would\n", g_ballast_kb,
               g_ballast ? "allocated" : "FAILED", prof);
    }
    g_self_us += now_us() - t0;
}

static void mem_probe(void)
{
    int f = probe_free_heap_kb();
    int hw = g_ballast ? f : f - g_ballast_kb;   /* with the ballast in place the emulator already behaves like hw */
    if (hw < g_mem_free_min_kb) g_mem_free_min_kb = hw;
    printf("PSPEST-MEM f=%u heap_free_emu_kb=%d heap_free_hw_kb=%d min_hw_kb=%d reserve_kb=%d%s\n", g_frame_no, f, hw,
           g_mem_free_min_kb, (int)PC(mem_reserve_kb), hw < PC(mem_reserve_kb) ? " BELOW_RESERVE" : "");
}

/* ------------------------------------------------------------------ init */
extern int sce_newlib_heap_kb_size __attribute__((weak));

void perf_init(void)
{
    if (g_inited) return;
    g_inited = 1;
    unsigned int t0 = now_us();
    memcpy(g_pc, g_pc_defaults, sizeof(g_pc));
    char found[64];
    cfg_load(found, sizeof(found));
    g_ge_log_draws = (int)PC(log_draws);
    g_ge_coverage = (int)PC(coverage);
    printf("PSPEST-INIT cfg=%s calibrated=%d cpu_k=%.2f overlap=%.2f coverage=%d heap_kb_setting=%d\n", found,
           (int)PC(calibrated), PC(cpu_k), PC(ge_overlap), g_ge_coverage,
           &sce_newlib_heap_kb_size ? sce_newlib_heap_kb_size : 0);
    g_self_us += now_us() - t0;
    mem_init();
    g_seg_start = now_us();
}

__attribute__((constructor)) static void perf_ctor(void) { perf_init(); }

/* ------------------------------------------------------------------ CPU sections / counters */
void perf_begin(int sec) { if (sec >= 0 && sec < PERF_NSEC) g_sec_t0[sec] = now_us(); }
void perf_end(int sec) { if (sec >= 0 && sec < PERF_NSEC) g_sec_us[sec] += now_us() - g_sec_t0[sec]; }
void perf_count(int what, int n) { if (what >= 0 && what < PERF_C_NCOUNT) g_counts[what] += n; }

/* ------------------------------------------------------------------ list hooks */
void perf_list_started(int cid, const void *list)
{
    g_cur_cid = cid;
    g_cur_list = list;
    if ((cid == 0 || cid == 2) && !g_list_seen) {
        g_list_seen = 1;
        unsigned int t = now_us();
        g_pre_us = g_cpu_us + (double)(t - g_seg_start) - g_self_us;
    }
}

void perf_list_finished(int size_bytes)
{
    if ((g_cur_cid == 0 || g_cur_cid == 2) && g_nq < MAXQ && g_cur_list) {
        g_q[g_nq].start = g_cur_list;
        g_q[g_nq].size = size_bytes;
        g_nq++;
    }
    g_cur_cid = -1;
}

static void overlay_draw(void);

/* called right before the real sceGuSync: closes the active CPU segment */
static void perf_sync_begin(void)
{
    unsigned int t = now_us();
    double seg = (double)(t - g_seg_start) - g_self_us;
    if (seg > 0) g_cpu_us += seg;
    g_self_us = 0;
}

/* analyze_every = N > 1: walk the lists (and log) only every Nth frame - long route runs stay fast */
static int analyzed_frame(void)
{
    unsigned int n = PC(analyze_every) > 1 ? (unsigned)PC(analyze_every) : 1u;
    return g_frame_no % n == 0;
}

void perf_ge_synced(void)
{
    unsigned int t0 = now_us();
    if (analyzed_frame())
        for (int i = 0; i < g_nq; i++) ge_scan_list(g_q[i].start, g_q[i].size, &g_fs);
    g_nq = 0;
    if (PC(overlay) > 0) overlay_draw();
    g_seg_start = now_us();    /* new active segment starts after our own work */
    (void)t0;
}

void perf_set_draw_buffer(int psm, void *fbp, int fbw)
{
    g_psm = psm; g_draw = fbp; g_fbw = fbw > 0 ? fbw : 512;
}

/* ------------------------------------------------------------------ estimate */
static int g_warn_n[16];
static void warn(int k, const char *kind, double v, double budget)
{
    int n = ++g_warn_n[k & 15];
    if (n <= 5 || n % 300 == 0)
        printf("PSPEST-WARN f=%u kind=%s value=%.1f budget=%.1f count=%d\n", g_frame_no, kind, v, budget, n);
}

static float vsync_fps(float ms)
{
    float per = 1000.0f / PC(vblank_hz);
    int n = (int)ceilf(ms / per - 0.001f);
    if (n < 1) n = 1;
    return PC(vblank_hz) / n;
}

void perf_frame_end(void *draw)
{
    unsigned int t0 = now_us();
    (void)draw;
    g_cpu_mhz = scePowerGetCpuClockFrequencyInt();
    g_bus_mhz = scePowerGetBusClockFrequencyInt();
    if (g_cpu_mhz <= 0) g_cpu_mhz = 222;
    if (g_bus_mhz <= 0) g_bus_mhz = 111;
    float k = PC(cpu_k);
    float cpu = (float)(g_cpu_us / 1000.0) * k;
    float pre = (float)((g_list_seen ? g_pre_us : g_cpu_us) / 1000.0) * k;
    float ge333 = (float)(g_fs.ns_total / 1e6);
    float ge = ge333 * PC(bus_target_mhz) / (float)g_bus_mhz;
    float frame = PC(frame_overlap) > 0 ? fmaxf(cpu, pre + ge) : cpu + ge;
    float cs = (float)g_cpu_mhz / PC(cpu_target_mhz);
    float cpu333 = cpu * cs, pre333 = pre * cs;
    float frame333 = PC(frame_overlap) > 0 ? fmaxf(cpu333, pre333 + ge333) : cpu333 + ge333;
    PerfFrame *L = &g_last;
    L->cpu_ms = cpu; L->ge_ms = ge; L->frame_ms = frame; L->fps = vsync_fps(frame);
    L->cpu333_ms = cpu333; L->ge333_ms = ge333; L->frame333_ms = frame333; L->fps333 = vsync_fps(frame333);
    L->tris = g_fs.tris - g_fs.tris_culled - g_fs.tris_clipped; L->draws = g_fs.prims; L->verts = g_fs.verts;
    L->px = (int)g_fs.px;
    int an = analyzed_frame();
    if (g_frame_no > 2 && an) {   /* the first frames include loading */
        if (frame333 > g_worst333) g_worst333 = frame333;
        g_sum333 += frame333;
        g_nframes++;
    }
    L->worst_frame333_ms = g_worst333;

    if (an && PC(log_every) > 0 && g_frame_no % (unsigned)PC(log_every) == 0) {
        char secs[160];
        int n = 0;
        secs[0] = 0;
        for (int i = 0; i < PERF_NSEC; i++)
            if (g_sec_us[i] > 0) n += snprintf(secs + n, sizeof(secs) - n, "%s%s:%.2f", n ? "," : "", g_sec_names[i], g_sec_us[i] / 1000.0 * k);
        unsigned int tn = now_us();
        printf("PSPEST f=%u wall=%.2f gewait=%.2f mhz=%d/%d cpu=%.2f pre=%.2f ge=%.2f frame=%.2f fps=%.1f cpu333=%.2f ge333=%.2f frame333=%.2f "
               "fps333=%.1f emu_cpu=%.2f tris=%d tris_in=%d culled=%d clipped=%d draws=%d skinned=%d verts=%d vs8=%d vs16=%d "
               "vf32=%d vthr=%d vskin=%d vlit=%d vbytes=%.0f px=%.0f od=%.2f texpx=%.0f texpx_ram=%.0f blendpx=%.0f clearpx=%.0f "
               "texb_ram=%.0f texb_vram=%.0f minif=%.1f texbind=%d texbind_ram=%d clut=%d clutb=%d bonemats=%d maxw=%d cmds=%d "
               "lists=%d ns_v=%.0f ns_p=%.0f ns_s=%.0f ns_c=%.0f chars=%d bones=%d coltests=%d secs=%s\n",
               g_frame_no, g_swap_t ? (tn - g_swap_t) / 1000.0 : 0.0, g_wait_us / 1000.0, g_cpu_mhz, g_bus_mhz, cpu, pre, ge, frame, L->fps, cpu333, ge333, frame333, L->fps333,
               g_cpu_us / 1000.0, L->tris, g_fs.tris, g_fs.tris_culled, g_fs.tris_clipped, g_fs.prims, g_fs.prims_skinned,
               g_fs.verts, g_fs.verts_fmt[VF_S8], g_fs.verts_fmt[VF_S16], g_fs.verts_fmt[VF_FLOAT], g_fs.verts_fmt[VF_THROUGH],
               g_fs.verts_skinned, g_fs.verts_lit, g_fs.vbytes + g_fs.ibytes, g_fs.px, g_fs.px / (480.0 * 272.0),
               g_fs.px_tex, g_fs.px_tex_ram, g_fs.px_blend, g_fs.px_clear, g_fs.tex_bytes_ram, g_fs.tex_bytes_vram,
               g_fs.px_tex > 0 ? g_fs.tex_ratio_sum / g_fs.px_tex : 0.0, g_fs.tex_binds, g_fs.tex_binds_ram, g_fs.clut_loads, g_fs.clut_bytes, g_fs.bone_mats,
               g_fs.max_weights, g_fs.cmds, g_fs.lists, g_fs.ns_vert, g_fs.ns_pix, g_fs.ns_state, g_fs.ns_cmd + g_fs.ns_list,
               g_counts[PERF_C_CHARS], g_counts[PERF_C_BONES], g_counts[PERF_C_COLTESTS], n ? secs : "-");
    }
    /* budgets (plan §4.3/§4.4) - estimated at 333 MHz */
    if (an && L->tris > PC(budget_tris)) warn(0, "tris", L->tris, PC(budget_tris));
    if (g_fs.prims > PC(budget_draws)) warn(1, "draw_calls", g_fs.prims, PC(budget_draws));
    if (g_fs.tex_binds > PC(budget_tex_changes)) warn(2, "tex_changes", g_fs.tex_binds, PC(budget_tex_changes));
    if (ge333 > PC(budget_ge_ms)) warn(3, "ge_ms@333", ge333, PC(budget_ge_ms));
    if (cpu333 > PC(budget_cpu_ms)) warn(4, "cpu_ms@333", cpu333, PC(budget_cpu_ms));
    if (g_fs.tex_over512) warn(5, "texture_over_512", g_fs.tex_over512, 0);
    if (g_fs.max_bones_draw > PC(budget_bones)) warn(6, "bones_per_draw", g_fs.max_bones_draw, PC(budget_bones));
    if (g_fs.tex_nonclut) warn(7, "texture_not_clut", g_fs.tex_nonclut, 0);
    if (g_cpu_mhz < PC(cpu_target_mhz) && g_frame_no > 2) warn(8, "cpu_clock_mhz", g_cpu_mhz, PC(cpu_target_mhz));
    if (frame333 > 1000.0f / 30.0f) warn(9, "frame_ms@333_over_33.3", frame333, 33.3);
    if (g_fs.unreadable) warn(10, "draws_unreadable", g_fs.unreadable, 0);
    if (PC(mem_probe_every) > 0 && g_frame_no % (unsigned)PC(mem_probe_every) == 0) {
        mem_probe();
        if (g_mem_free_min_kb < PC(mem_reserve_kb)) warn(11, "free_heap_kb_hw", g_mem_free_min_kb, PC(mem_reserve_kb));
    }
    if (g_frame_no % 300 == 299 && g_nframes)
        printf("PSPEST-S frames=%u avg_frame333=%.2f fps333_avg=%.1f worst_frame333=%.2f\n", g_nframes, g_sum333 / g_nframes,
               1000.0 / (g_sum333 / g_nframes), g_worst333);

    /* next frame */
    ge_stats_clear(&g_fs);
    memset(g_sec_us, 0, sizeof(g_sec_us));
    memset(g_counts, 0, sizeof(g_counts));
    g_cpu_us = 0; g_pre_us = 0; g_list_seen = 0; g_wait_us = 0;
    g_swap_t = now_us();
    g_frame_no++;
    g_self_us = 0;
    (void)t0;
}

/* called after the real swap: the new frame's CPU segment starts now */
static void perf_frame_start(void) { g_seg_start = now_us(); g_self_us = 0; }

const PerfFrame *perf_last(void) { return &g_last; }

/* ------------------------------------------------------------------ overlay (msx 8x8 font from libpspdebug) */
extern unsigned char msx[];

static void ov_text(int x, int y, const char *s, unsigned int col565, unsigned int col8888)
{
    if (!g_draw) return;
    unsigned int base = 0x44000000 | (unsigned int)g_draw;
    int is32 = g_psm == 3;
    for (; *s; s++, x += 8) {
        if (x + 8 > 480) break;
        const unsigned char *g = msx + (unsigned char)*s * 8;
        for (int r = 0; r < 8; r++) {
            unsigned char bits = g[r];
            for (int c = 0; c < 8; c++) {
                int on = bits & (0x80 >> c);
                int off = (y + r) * g_fbw + x + c;
                if (is32) ((unsigned int *)base)[off] = on ? col8888 : 0xff000000;
                else ((unsigned short *)base)[off] = on ? (unsigned short)col565 : 0;
            }
        }
    }
}

static void overlay_draw(void)
{
    if (g_frame_no < 1) return;
    const PerfFrame *L = &g_last;
    char a[64], b[64];
    snprintf(a, sizeof(a), "PSP-est %2.0ffps %5.1fms cpu%5.1f ge%5.1f w%5.1f", L->fps333, L->frame333_ms, L->cpu333_ms,
             L->ge333_ms, L->worst_frame333_ms);
    snprintf(b, sizeof(b), "%5dtri %4.1fxpx %3ddc %s%s%s", L->tris, L->px / (480.0f * 272.0f), L->draws,
             PC(calibrated) > 0 ? "cal" : "UNCAL", g_cpu_mhz < 333 ? " CPU<333!" : "", g_warn_n[5] ? " TEX>512!" : "");
    unsigned int col = L->fps333 >= 29.5f ? 0x07E0 : L->fps333 >= 25.0f ? 0x07FF : 0x001F;     /* 5650: green/yellow/red */
    unsigned int col32 = L->fps333 >= 29.5f ? 0xff00ff00 : L->fps333 >= 25.0f ? 0xff00ffff : 0xff0000ff;
    ov_text(0, 272 - 16, a, col, col32);
    ov_text(0, 272 - 8, b, 0xFFFF, 0xffffffff);
}

/* ------------------------------------------------------------------ --wrap hooks (zero-touch integration) */
int __real_sceGuStart(int cid, void *list);
int __real_sceGuFinish(void);
int __real_sceGuSync(int mode, int what);
void *__real_sceGuSwapBuffers(void);
void __real_sceGuDrawBuffer(int psm, void *fbp, int fbw);

int __wrap_sceGuStart(int cid, void *list)
{
    int r = __real_sceGuStart(cid, list);
    perf_list_started(cid, list);
    return r;
}

int __wrap_sceGuFinish(void)
{
    int r = __real_sceGuFinish();
    perf_list_finished(r);
    return r;
}

int __wrap_sceGuSync(int mode, int what)
{
    perf_sync_begin();
    unsigned int t0 = now_us();
    int r = __real_sceGuSync(mode, what);
    g_wait_us += now_us() - t0;
    perf_ge_synced();
    return r;
}

void *__wrap_sceGuSwapBuffers(void)
{
    perf_frame_end(g_draw);
    void *r = __real_sceGuSwapBuffers();
    g_draw = r;
    perf_frame_start();
    return r;
}

void __wrap_sceGuDrawBuffer(int psm, void *fbp, int fbw)
{
    __real_sceGuDrawBuffer(psm, fbp, fbw);
    perf_set_draw_buffer(psm, fbp, fbw);
}
