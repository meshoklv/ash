/* src/diag - black box log, hang watchdog, crash catcher for the real PSP. See diag.h, docs/hw-pitfalls.md. */
#include "diag.h"

#include <kubridge.h>
#include <malloc.h>
#include <pspctrl.h>
#include <pspdisplay.h>
#include <pspge.h>
#include <pspgu.h>
#include <pspiofilemgr.h>
#include <pspkernel.h>
#include <psprtc.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define RING_N 256
#define RING_W 240
#define LOG_MAX (1024 * 1024)        /* log.txt over 1 MB -> log_old.txt (keeps ~2 h of play) */
#define HANG_MS 3000

static char g_dir[160];
static char g_build[24];
static char g_ring[RING_N][RING_W];
static volatile u32 g_head, g_tail, g_dropped;   /* lines written / flushed; SPSC under g_sema */
static SceUID g_sema = -1;
static volatile int g_urgent;
static volatile u32 g_hb;
static const char *volatile g_stage = "boot";
static volatile int g_stage_ms = HANG_MS;
static volatile int *g_running;
static SceUID g_main_tid = -1;
static u64 g_t0;
static volatile u32 g_frame;
static DiagStatFn g_stat_fn;
static u32 g_heap_peak;
static int g_log_size;
static int g_exc_state = -1000;   /* result of the handler install (0 ok) */
static AshExcRegs __attribute__((aligned(16))) g_exc;
static u8 __attribute__((aligned(16))) g_park_stack[48 * 1024];
static volatile int g_parked;

/* ------------------------------------------------------------------ small helpers (no malloc, no float) */

static u64 now_us(void) { return (u64)sceKernelGetSystemTimeWide(); }
static u32 since_ms(void) { return (u32)((now_us() - g_t0) / 1000); }

static void path(char *out, int n, const char *name) { snprintf(out, n, "%s%s", g_dir, name); }

typedef struct { char *p; int n, cap; } Sb;
static void sb_s(Sb *b, const char *s) { while (s && *s && b->n < b->cap - 1) b->p[b->n++] = *s++; b->p[b->n] = 0; }
static void sb_hex(Sb *b, u32 v)
{
    char t[11] = "0x";
    for (int i = 0; i < 8; i++) t[2 + i] = "0123456789abcdef"[(v >> (28 - 4 * i)) & 15];
    t[10] = 0;
    sb_s(b, t);
}
static void sb_u(Sb *b, u32 v)
{
    char t[12];
    int i = 11;
    t[i] = 0;
    do { t[--i] = (char)('0' + v % 10); v /= 10; } while (v && i > 0);
    sb_s(b, t + i);
}

static void file_write(const char *name, const char *data, int len, int append, int sync)
{
    char p[200];
    path(p, sizeof p, name);
    SceUID fd = sceIoOpen(p, PSP_O_WRONLY | PSP_O_CREAT | (append ? PSP_O_APPEND : PSP_O_TRUNC), 0777);
    if (fd < 0) return;
    sceIoWrite(fd, data, len);
    sceIoClose(fd);
    if (sync) sceIoSync(g_dir[0] == 'e' ? "ef0:" : "ms0:", 0);
}

static int file_exists(const char *name)
{
    char p[200];
    SceIoStat st;
    path(p, sizeof p, name);
    return sceIoGetstat(p, &st) >= 0;
}

static void file_rotate(const char *name, const char *to)
{
    if (!file_exists(name)) return;
    char a[200], b[200];
    path(a, sizeof a, name);
    path(b, sizeof b, to);
    sceIoRemove(b);
    sceIoRename(a, b);
}

/* ------------------------------------------------------------------ ring */

/* lock with a timeout: a hung/crashed holder must not block the writer thread forever */
static int lock(void)
{
    SceUInt to = 200000;
    return g_sema >= 0 && sceKernelWaitSema(g_sema, 1, &to) >= 0;
}
static void unlock(int l) { if (l) sceKernelSignalSema(g_sema, 1); }

static void ring_put(const char *line)
{
    int l = lock();
    if (g_head - g_tail >= RING_N) { g_tail++; g_dropped++; }   /* writer is far behind: drop the oldest */
    char *d = g_ring[g_head % RING_N];
    strncpy(d, line, RING_W - 2);
    d[RING_W - 2] = 0;
    g_head++;
    unlock(l);
}

static void vevent(int sync, const char *fmt, va_list ap)
{
    char line[RING_W];
    u32 ms = since_ms();
    int o = snprintf(line, sizeof line, "[%u.%u f%u] ", (unsigned)(ms / 1000), (unsigned)(ms / 100 % 10),
                     (unsigned)g_frame);
    vsnprintf(line + o, sizeof line - o, fmt, ap);
    ring_put(line);
    if (sync) g_urgent = 1;
}

void diag_event(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vevent(0, fmt, ap);
    va_end(ap);
}

void diag_event_sync(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vevent(1, fmt, ap);
    va_end(ap);
}

/* writer side (diag thread): pending lines -> log.txt */
static void flush_ring(int sync)
{
    static char out[RING_N * 40];   /* lines are short; one write per call, the rest next call */
    int n = 0;
    int lk = lock();
    u32 dropped = g_dropped;
    g_dropped = 0;
    while (g_tail != g_head) {
        const char *s = g_ring[g_tail % RING_N];
        int l = (int)strlen(s);
        if (n + l + 1 > (int)sizeof out) break;
        memcpy(out + n, s, l);
        n += l;
        out[n++] = '\n';
        g_tail++;
    }
    unlock(lk);
    if (dropped) {
        char t[48];
        int l = snprintf(t, sizeof t, "(%u lines dropped)\n", (unsigned)dropped);
        file_write("log.txt", t, l, 1, 0);
    }
    if (!n) return;
    if (g_log_size + n > LOG_MAX) { file_rotate("log.txt", "log_old.txt"); g_log_size = 0; }
    file_write("log.txt", out, n, 1, sync);
    g_log_size += n;
}

/* ------------------------------------------------------------------ screen text (CPU into the shown framebuffer) */

extern u8 msx[];   /* pspdebug's 8x8 font */
static const struct { u16 cp; u8 g[8]; } k_cyr[] = {
    {0x0417, {0x78, 0x84, 0x04, 0x38, 0x04, 0x84, 0x78, 0}},   /* З */
    {0x0421, {0x78, 0x84, 0x80, 0x80, 0x80, 0x84, 0x78, 0}},   /* С */
    {0x0430, {0, 0, 0x78, 0x04, 0x7C, 0x84, 0x7C, 0}},         /* а */
    {0x0431, {0x7C, 0x80, 0xF8, 0x84, 0x84, 0x84, 0x78, 0}},   /* б */
    {0x0432, {0, 0, 0xF8, 0x84, 0xF8, 0x84, 0xF8, 0}},         /* в */
    {0x0435, {0, 0, 0x78, 0x84, 0xFC, 0x80, 0x78, 0}},         /* е */
    {0x0438, {0, 0, 0x84, 0x8C, 0x94, 0xA4, 0xC4, 0}},         /* и */
    {0x0439, {0x48, 0x30, 0x84, 0x8C, 0x94, 0xA4, 0xC4, 0}},   /* й */
    {0x043D, {0, 0, 0x84, 0x84, 0xFC, 0x84, 0x84, 0}},         /* н */
    {0x043E, {0, 0, 0x78, 0x84, 0x84, 0x84, 0x78, 0}},         /* о */
    {0x0441, {0, 0, 0x78, 0x84, 0x80, 0x84, 0x78, 0}},         /* с */
};

typedef struct { u8 *base; int bw, fmt; } Fb;

static int fb_get(Fb *f)
{
    void *top = NULL;
    int bw = 0, fmt = 0;
    sceDisplayGetFrameBuf(&top, &bw, &fmt, PSP_DISPLAY_SETBUF_IMMEDIATE);
    if (!top || bw <= 0) return 0;
    f->base = (u8 *)(0x40000000u | ((u32)top & 0x1FFFFFFFu));
    f->bw = bw;
    f->fmt = fmt;
    return 1;
}

static void fb_px(const Fb *f, int x, int y, u32 abgr)
{
    if (x < 0 || y < 0 || x >= 480 || y >= 272) return;
    if (f->fmt == PSP_DISPLAY_PIXEL_FORMAT_8888) ((u32 *)f->base)[y * f->bw + x] = abgr;
    else {
        u32 r = abgr & 0xff, g = (abgr >> 8) & 0xff, b = (abgr >> 16) & 0xff;
        ((u16 *)f->base)[y * f->bw + x] = (u16)((r >> 3) | ((g >> 2) << 5) | ((b >> 3) << 11));
    }
}

static void fb_rect(const Fb *f, int x, int y, int w, int h, u32 c)
{
    for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) fb_px(f, x + i, y + j, c);
}

/* UTF-8 text, scale 1 or 2; returns the x after it */
static int fb_text(const Fb *f, int x, int y, int sc, const char *s, u32 c)
{
    const u8 *p = (const u8 *)s;
    while (*p) {
        u32 cp = *p++;
        if (cp >= 0xC0 && (*p & 0xC0) == 0x80) { cp = ((cp & 0x1F) << 6) | (*p++ & 0x3F); }
        const u8 *g = NULL;
        if (cp < 128) g = &msx[cp * 8];
        else for (unsigned k = 0; k < sizeof k_cyr / sizeof k_cyr[0]; k++) if (k_cyr[k].cp == cp) g = k_cyr[k].g;
        if (!g) g = &msx['?' * 8];
        for (int r = 0; r < 8; r++)
            for (int b = 0; b < 8; b++)
                if (g[r] & (0x80 >> b)) fb_rect(f, x + b * sc, y + r * sc, sc, sc, c);
        x += 8 * sc;
    }
    return x;
}

static void screen_msg(const char *head, const char *l1, const char *l2, const char *l3)
{
    Fb f;
    if (!fb_get(&f)) return;
    fb_rect(&f, 0, 0, 480, 92, 0xff101018);
    fb_text(&f, 12, 8, 2, head, 0xff4040ff);
    if (l1) fb_text(&f, 12, 34, 1, l1, 0xffffffff);
    if (l2) fb_text(&f, 12, 48, 1, l2, 0xffffffff);
    if (l3) fb_text(&f, 12, 66, 1, l3, 0xff80ffff);
}

/* ------------------------------------------------------------------ crash: park function (crashed thread) */

static const char *exc_name(u32 code)
{
    static const char *const n[16] = {"Int", "Mod", "TLBL", "TLBS", "AdEL (bad address load/fetch)",
                                      "AdES (bad address store)", "IBE (bus error, fetch)", "DBE (bus error, data)",
                                      "Sys", "Bp (break: division by zero?)", "RI (reserved instruction)",
                                      "CpU (coprocessor unusable: VFPU/FPU attr?)", "Ov", "Tr (trap)", "14",
                                      "FPE"};
    return code < 16 ? n[code] : "?";
}

static int is_text(u32 a)
{
    extern char __executable_start[], _etext[];
    return a >= (u32)__executable_start && a < (u32)_etext && !(a & 3);
}

static void park_write(AshExcRegs *x)
{
    static char buf[16 * 1024];
    static const char *const rn[32] = {"zr", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2",
                                       "t3", "t4", "t5", "t6", "t7", "s0", "s1", "s2", "s3", "s4", "s5",
                                       "s6", "s7", "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra"};
    Sb b = {buf, 0, sizeof buf};
    u32 code = (x->cause >> 2) & 31;
    sb_s(&b, "ASH CRASH  build ");
    sb_s(&b, g_build);
    sb_s(&b, x->soft ? "  (SOFTWARE TEST, not a CPU exception)" : "");
    sb_s(&b, "\nt_ms=");
    sb_u(&b, since_ms());
    sb_s(&b, " frame=");
    sb_u(&b, g_frame);
    sb_s(&b, " stage=");
    sb_s(&b, g_stage);
    sb_s(&b, "\nexception: ");
    sb_s(&b, exc_name(code));
    sb_s(&b, "\nEPC=");
    sb_hex(&b, x->epc);
    sb_s(&b, " BadVAddr=");
    sb_hex(&b, x->badvaddr);
    sb_s(&b, " Cause=");
    sb_hex(&b, x->cause);
    sb_s(&b, " Status=");
    sb_hex(&b, x->status);
    sb_s(&b, x->cause & 0x80000000u ? " (in branch delay slot: fault at EPC+4)" : "");
    SceKernelThreadInfo ti;
    memset(&ti, 0, sizeof ti);
    ti.size = sizeof ti;
    sceKernelReferThreadStatus(0, &ti);
    sb_s(&b, "\nthread: ");
    sb_s(&b, ti.name);
    sb_s(&b, " stack=");
    sb_hex(&b, (u32)ti.stack);
    sb_s(&b, " size=");
    sb_u(&b, (u32)ti.stackSize);
    sb_s(&b, x->r[29] < (u32)ti.stack + 256 && x->r[29] >= (u32)ti.stack - 4096 ? "  sp AT STACK BOTTOM: overflow?" : "");
    for (int i = 0; i < 32; i++) {
        sb_s(&b, i % 4 ? "  " : "\n");
        sb_s(&b, rn[i]);
        sb_s(&b, "=");
        sb_hex(&b, x->r[i]);
    }
    sb_s(&b, "\nlo=");
    sb_hex(&b, x->lo);
    sb_s(&b, " hi=");
    sb_hex(&b, x->hi);
    {
        extern char __executable_start[], _etext[];
        sb_s(&b, "\ntext=");
        sb_hex(&b, (u32)__executable_start);
        sb_s(&b, "..");
        sb_hex(&b, (u32)_etext);
    }
    /* call chain: EPC, ra, then words on the stack that are return addresses (the word 8 bytes before is a jal/jalr) */
    sb_s(&b, "\ntrace: ");
    sb_hex(&b, x->epc);
    sb_s(&b, " ");
    sb_hex(&b, x->r[31]);
    u32 sp = x->r[29] & ~3u, top = (u32)ti.stack + (u32)ti.stackSize;
    if (!ti.stack || sp < (u32)ti.stack || sp >= top) top = sp + 4096;   /* unknown stack: 4 KB */
    if (top > sp + 32768) top = sp + 32768;
    int nt = 0;
    if ((sp >= 0x08800000u && sp < 0x0C000000u) || (sp >= 0x48800000u && sp < 0x4C000000u))
        for (u32 a = sp; a < top && nt < 32; a += 4) {
            u32 w = *(volatile u32 *)a;
            if (!is_text(w) || !is_text(w - 8)) continue;
            u32 ins = *(volatile u32 *)(w - 8);
            if ((ins >> 26) != 3 && !((ins >> 26) == 0 && (ins & 63) == 9)) continue;
            sb_s(&b, " ");
            sb_hex(&b, w);
            nt++;
        }
    sb_s(&b, "\nstack @sp:");
    if ((sp >= 0x08800000u && sp < 0x0C000000u))
        for (int i = 0; i < 32; i++) {
            sb_s(&b, i % 8 ? " " : "\n  ");
            sb_hex(&b, *(volatile u32 *)(sp + 4 * i));
        }
    sb_s(&b, "\nexc handler: ");
    sb_s(&b, g_exc_state == 0 ? "installed" : "NOT installed");
    sb_s(&b, "\ndecode: python3 tools/crash_decode.py crash.txt --map debug/ash_ds.map\n--- last log lines ---\n");
    u32 h = g_head, from = h > 48 ? h - 48 : 0;
    if (h - from > RING_N) from = h - RING_N;
    for (u32 i = from; i < h; i++) { sb_s(&b, g_ring[i % RING_N]); sb_s(&b, "\n"); }
    file_write("crash.txt", buf, b.n, 0, 1);
}

static void park_loop(void)
{
    for (;;) {
        SceCtrlData pad;
        sceCtrlPeekBufferPositive(&pad, 1);
        if ((pad.Buttons & PSP_CTRL_START) || (g_running && !*g_running)) sceKernelExitGame();
        sceKernelDelayThread(50000);
    }
}

void diag_park(AshExcRegs *x)
{
    if (g_parked++) park_loop();   /* a second fault while writing: just wait */
    park_write(x);
    {
        char t[64];
        int l = snprintf(t, sizeof t, "CRASH (exception %u) -> crash.txt\n", (unsigned)((x->cause >> 2) & 31));
        flush_ring(0);
        file_write("log.txt", t, l, 1, 1);
    }
    char l1[64], l2[64];
    Sb b1 = {l1, 0, sizeof l1}, b2 = {l2, 0, sizeof l2};
    sb_s(&b1, "EPC ");
    sb_hex(&b1, x->epc);
    sb_s(&b1, "  addr ");
    sb_hex(&b1, x->badvaddr);
    sb_s(&b2, exc_name((x->cause >> 2) & 31));
    for (int k = 0; k < 5; k++) {   /* the GE may still flip a buffer: draw a few times */
        screen_msg("Сбой", l1, l2, "crash.txt saved. START = exit");
        sceKernelDelayThread(200000);
    }
    park_loop();
}

/* ------------------------------------------------------------------ watchdog / writer thread */

static void report_hang(u32 still_ms)
{
    SceKernelThreadInfo a, b;
    memset(&a, 0, sizeof a);
    a.size = sizeof a;
    b = a;
    sceKernelReferThreadStatus(g_main_tid, &a);
    sceKernelDelayThread(100000);
    sceKernelReferThreadStatus(g_main_tid, &b);
    char gst[RING_W * 2];
    int ge = diag_ge_state(gst, sizeof gst);   /* drawsync peek: 0 done, 1 queued, 2 drawing, 3 stall, <0 error */
    u32 run_a = a.runClocks.low, run_b = b.runClocks.low;
    diag_event_sync("HANG stage=%s no frame for %u ms; main thread status=%d wait_type=%d wait_id=%x prio=%d "
                    "run_us+%u in 100ms (%s); GE drawsync(peek)=%d; heap %u KB",
                    g_stage, (unsigned)still_ms, b.status, b.waitType, (unsigned)b.waitId, b.currentPriority,
                    (unsigned)(run_b - run_a), run_b - run_a > 50000 ? "SPINNING on CPU" : "waiting",
                    ge, (unsigned)(diag_heap_used() / 1024));
    diag_event_sync("HANG GE state: %s", gst);
    flush_ring(1);
    char l1[96], l2[96];
    snprintf(l1, sizeof l1, "HANG f%u, %u s, GE=%d, main %s", (unsigned)g_frame, (unsigned)(still_ms / 1000), ge,
             run_b - run_a > 50000 ? "spins" : "waits");
    snprintf(l2, sizeof l2, "log.txt saved. HOME = exit");
    char head[96];
    snprintf(head, sizeof head, "Зависание: %.24s", g_stage);
    screen_msg(head, l1, l2, NULL);
}

static int diag_thread(SceSize args, void *argp)
{
    (void)args; (void)argp;
    u32 last_hb = g_hb;
    u64 last_change = now_us(), last_flush = now_us();
    int hung = 0;
    for (;;) {
        sceKernelDelayThread(250000);
        if (g_parked) continue;
        u64 t = now_us();
        if (g_hb != last_hb) {
            last_hb = g_hb;
            last_change = t;
            if (hung) { diag_event("RESUMED after the hang (stage %s)", g_stage); hung = 0; }
        } else {
            u32 still = (u32)((t - last_change) / 1000);
            if (!hung && still > (u32)g_stage_ms) { report_hang(still); hung = 1; }
            if (hung && g_running && !*g_running) { flush_ring(1); sceKernelExitGame(); }
        }
        if (g_urgent || t - last_flush >= 1000000) {
            int s = g_urgent;
            g_urgent = 0;
            flush_ring(s);
            last_flush = t;
        }
    }
    return 0;
}


/* ------------------------------------------------------------------ GE wait with a timeout */

static volatile u32 g_ge_resets;
u32 diag_ge_resets(void) { return g_ge_resets; }

int diag_ge_state(char *buf, int n)
{
    static const int cmds[] = {0x01, 0x02, 0x04, 0x10, 0x12, 0x9C, 0x9E, 0xA0, 0xA8, 0xB0, 0xB1, 0xB8, 0xC2, 0xC3, 0xC4,
                               0xC5};
    static const char *const nm[] = {"vaddr", "iaddr", "prim", "base", "vtype", "fbp", "zbp", "tex0", "tbw0", "cbp",
                                     "cbw", "tsize0", "tmode", "tpsm", "cload", "cmode"};
    int ds = sceGeDrawSync(1);
    int o = snprintf(buf, n, "drawsync(peek)=%d", ds);
    for (unsigned i = 0; i < sizeof cmds / sizeof cmds[0] && o < n - 24; i++)
        o += snprintf(buf + o, n - o, " %s=%08x", nm[i], (unsigned)sceGeGetCmd(cmds[i]));
    PspGeStack st;
    int depth = sceGeGetStack(-1, NULL);
    o += snprintf(buf + o, n - o, " stack_depth=%d", depth);
    if (depth > 0 && sceGeGetStack(0, &st) >= 0 && o < n - 40)
        o += snprintf(buf + o, n - o, " stack0=%08x %08x %08x", st.stack[0], st.stack[1], st.stack[2]);
    return ds;
}

int diag_ge_wait(const char *what, int timeout_ms, int spin)
{
    u64 t0 = now_us(), lim = (u64)timeout_ms * 1000;
    for (;;) {
        int r = sceGeDrawSync(1);
        if (r == PSP_GE_LIST_DONE || r == PSP_GE_LIST_CANCEL_DONE || r < 0) return 0;
        if (now_us() - t0 > lim) break;
        if (!spin) sceKernelDelayThread(100);
    }
    char st[RING_W * 2];
    diag_ge_state(st, sizeof st);
    PspGeBreakParam bp;
    memset(&bp, 0, sizeof bp);
    int rb = sceGeBreak(1, &bp);   /* reset all queues: the waiting code goes on */
    g_ge_resets++;
    diag_event_sync("GE HANG in '%s' (stage %s): no finish in %d ms -> GE reset %x", what, g_stage, timeout_ms,
                    (unsigned)rb);
    diag_event_sync("GE state: %s", st);
    return -1;
}

/* ------------------------------------------------------------------ init, stages, frames */

extern void diag_park(AshExcRegs *x);

static void load_exc_prx(void)
{
    char p[200];
    path(p, sizeof p, "ashexc.prx");
    g_exc.installed = 0xFFFFFFFFu;
    SceUID m = kuKernelLoadModule(p, 0, NULL);
    if (m < 0) { g_exc_state = m; diag_event("exc handler: kuKernelLoadModule(%s) = %x -> unavailable", p, (unsigned)m); return; }
    u32 args[3] = {(u32)&g_exc, (u32)diag_park, ((u32)(g_park_stack + sizeof g_park_stack) - 64) & ~15u};
    int st = 0;
    int r = sceKernelStartModule(m, sizeof args, args, &st, NULL);
    g_exc_state = r < 0 ? r : (int)g_exc.installed;
    diag_event("exc handler: module %x start %x, register %x -> %s", (unsigned)m, (unsigned)r,
               (unsigned)g_exc.installed, g_exc_state == 0 ? "INSTALLED" : "unavailable");
}

void diag_init(const char *argv0, const char *build)
{
    g_t0 = now_us();
    g_main_tid = sceKernelGetThreadId();
    snprintf(g_build, sizeof g_build, "%s", build ? build : "?");
    g_dir[0] = 0;
    if (argv0) {
        const char *sl = strrchr(argv0, '/');
        if (sl && sl - argv0 + 2 < (int)sizeof g_dir) { memcpy(g_dir, argv0, sl - argv0 + 1); g_dir[sl - argv0 + 1] = 0; }
    }
    file_rotate("log.txt", "log_prev.txt");
    file_rotate("crash.txt", "crash_prev.txt");
    g_sema = sceKernelCreateSema("ash_diag", 0, 1, 1, NULL);
    ScePspDateTime tm;
    memset(&tm, 0, sizeof tm);
    sceRtcGetCurrentClockLocalTime(&tm);
    char h[256];
    int l = snprintf(h, sizeof h, "=== Ash log  build %s  %04d-%02d-%02d %02d:%02d:%02d  fw %x  model %d  dir '%s'\n",
                     g_build, tm.year, tm.month, tm.day, tm.hour, tm.minute, tm.second,
                     (unsigned)sceKernelDevkitVersion(), kuKernelGetModel(), g_dir);
    file_write("log.txt", h, l, 0, 0);
    g_log_size = l;
    load_exc_prx();
    diag_event("boot: kernel free %u KB, max block %u KB, heap used %u KB (arena %u KB)",
               (unsigned)(sceKernelTotalFreeMemSize() / 1024), (unsigned)(sceKernelMaxFreeMemSize() / 1024),
               (unsigned)(diag_heap_used() / 1024), (unsigned)(mallinfo().arena / 1024));
    SceUID th = sceKernelCreateThread("ash_diag", diag_thread, 0x18, 0x4000, PSP_THREAD_ATTR_USER, NULL);
    if (th >= 0) sceKernelStartThread(th, 0, NULL);
    else diag_event("diag thread: create failed %x", (unsigned)th);
    flush_ring(1);
}

void diag_set_exit_flag(volatile int *running) { g_running = running; }
void diag_set_stat_fn(DiagStatFn fn) { g_stat_fn = fn; }
const char *diag_cur_stage(void) { return g_stage; }

void diag_stage_t(const char *stage, int timeout_ms)
{
    g_stage_ms = timeout_ms > 0 ? timeout_ms : HANG_MS;
    g_stage = stage;
    g_hb++;
}

void diag_stage(const char *stage) { diag_stage_t(stage, 0); }
void diag_beat(void) { g_hb++; }

u32 diag_heap_used(void)
{
    u32 u = (u32)mallinfo().uordblks;
    if (u > g_heap_peak) g_heap_peak = u;
    return u;
}
u32 diag_heap_peak(void) { return g_heap_peak; }

void diag_frame(u32 frame)
{
    static u64 last, next_stat;
    static u32 sum, mx, n, slow;
    u64 t = now_us();
    g_frame = frame;
    g_hb++;
    if (last) {
        u32 dt = (u32)(t - last);
        sum += dt;
        if (dt > mx) mx = dt;
        if (dt > 50000) slow++;
        n++;
    }
    last = t;
    if (t < next_stat) return;
    next_stat = t + 1000000;
    if (!n) return;
    char line[RING_W], game[RING_W];
    game[0] = 0;
    if (g_stat_fn) g_stat_fn(game, sizeof game);
    u32 heap = diag_heap_used();
    snprintf(line, sizeof line, "STAT fps=%.1f ft=%.1f/%.1fms slow=%u heap=%u/%uKB free=%uKB %s",
             n * 1e6f / (float)sum, sum / 1000.0f / n, mx / 1000.0f, (unsigned)slow, (unsigned)(heap / 1024),
             (unsigned)(g_heap_peak / 1024), (unsigned)(sceKernelMaxFreeMemSize() / 1024), game);
    diag_event("%s", line);
    sum = mx = n = slow = 0;
}

/* ------------------------------------------------------------------ malloc tracking (-Wl,--wrap=...) */

void *__real_malloc(size_t n);
void *__real_calloc(size_t a, size_t b);
void *__real_realloc(void *p, size_t n);
void *__real_memalign(size_t a, size_t n);

static void *track(void *p, size_t n, const char *what)
{
    if (!p && n) {
        diag_event_sync("MALLOC FAIL %s %u B (heap used %u KB, peak %u KB, max free block %u KB)", what,
                        (unsigned)n, (unsigned)(diag_heap_used() / 1024), (unsigned)(g_heap_peak / 1024),
                        (unsigned)(sceKernelMaxFreeMemSize() / 1024));
    } else if (n >= 64 * 1024) {
        diag_event("%s %u KB -> %08x (heap %u KB)", what, (unsigned)(n / 1024), (unsigned)(u32)p,
                   (unsigned)(diag_heap_used() / 1024));
    }
    return p;
}

void *__wrap_malloc(size_t n) { return track(__real_malloc(n), n, "malloc"); }
void *__wrap_calloc(size_t a, size_t b) { return track(__real_calloc(a, b), a * b, "calloc"); }
void *__wrap_realloc(void *p, size_t n) { return track(__real_realloc(p, n), n, "realloc"); }
void *__wrap_memalign(size_t a, size_t n) { return track(__real_memalign(a, n), n, "memalign"); }

/* ------------------------------------------------------------------ tests */

extern void diag_soft_snapshot(AshExcRegs *x);                 /* diag_asm.S: GPRs + epc = caller */
extern void diag_jump_park(AshExcRegs *x, void *fn, u32 stack);  /* diag_asm.S: sp = stack, jump fn(x) */

void diag_test_crash(int kind)
{
    diag_event_sync("TEST: crash kind %d (0 cpu exception, 1 software, 2 hang)", kind);
    sceKernelDelayThread(1200000);   /* let the writer put the line out */
    if (kind == 1) {
        diag_soft_snapshot(&g_exc);
        g_exc.cause = 7u << 2;   /* DBE */
        g_exc.badvaddr = 0;
        g_exc.soft = 1;
        g_exc.magic = 0xA5EC0001u;
        diag_jump_park(&g_exc, (void *)diag_park, ((u32)(g_park_stack + sizeof g_park_stack) - 64) & ~15u);
    } else if (kind == 2) {
        diag_stage("TEST hang (spin)");
        for (volatile int i = 0;; i++) {}
    } else {
        volatile u32 *volatile bad = (volatile u32 *)0x00000004u;   /* unmapped: bus error on the PSP */
        *bad = 0xDEADBEEFu;
    }
}
