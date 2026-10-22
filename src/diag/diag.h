/*
 * src/diag - diagnostics for the real PSP (docs/hw-pitfalls.md, the hw-day1 design notes). All files land next to the EBOOT
 * (ms0:/PSP/GAME/ASHGAME/):
 *
 *   log.txt   "black box": a line per ~1 s (frame, frame time, heap, free memory, GE, game state) + key events
 *             (zone load, menus, boss, death, save, malloc >= 64 KB, malloc failures). The main thread only appends
 *             to a RAM ring; the diag thread writes it out once a second (open-append-close, ~1 write/s).
 *             On boot the previous log.txt becomes log_prev.txt (the one of the crashed run survives one relaunch).
 *   crash.txt CPU exception: registers, EPC, BadVAddr, Cause, return addresses found on the stack, the last log
 *             lines. Needs ashexc.prx (kernel helper, loaded through KUBridge of PRO/ARK): without it the PSP has
 *             no user-mode way to catch an exception, the log says "exc handler: unavailable".
 *             Decode: tools/crash_decode.py crash.txt --map <dist>/debug/ash_ds.map
 *   HANG      diag thread (higher priority than the game) sees no heartbeat for > 3 s (or the stage's own limit):
 *             "HANG" + stage + main thread state + GE state into log.txt, "Зависание: <stage>" on screen.
 *
 * Emulator: PPSSPP never delivers CPU exceptions to the game, so crash.txt there comes only from diag_test_crash(1)
 * (a software snapshot of the registers through the same park path). diag_test_crash(0) is a real null store
 * (hardware test).
 */
#ifndef ASH_DIAG_H
#define ASH_DIAG_H

#include <psptypes.h>

/* register block filled by ashexc.prx (layout fixed: src/diag/exc/ashexc_asm.S) */
typedef struct {
    u32 magic;        /* 0xA5EC0001 when filled */
    u32 installed;    /* result of sceKernelRegisterDefaultExceptionHandler (0 = ok) */
    u32 r[32];
    u32 epc, cause, badvaddr, status, lo, hi;
    u32 soft;         /* 1 = software snapshot (diag_test_crash), not a CPU exception */
} AshExcRegs;

/* argv0 = path of the EBOOT (main's argv[0]): files go to its folder; NULL -> current folder.
   build = short git sha for the log header. */
void diag_init(const char *argv0, const char *build);
void diag_set_exit_flag(volatile int *running);            /* HOME -> exit also from a hang / crash screen */

/* stage = static ASCII string: where the main thread is. timeout_ms = hang limit while in this stage (0 = 3000). */
void diag_stage(const char *stage);
void diag_stage_t(const char *stage, int timeout_ms);
const char *diag_cur_stage(void);

/* once per presented frame from the main loop: heartbeat + frame time; once a second it appends the stat line,
   built by the callback (main thread; may use floats and snprintf). */
typedef int (*DiagStatFn)(char *buf, int n);
void diag_set_stat_fn(DiagStatFn fn);
void diag_frame(u32 frame);
void diag_beat(void);                                      /* progress without a frame (long loads) */

/* event line (main thread): "[t=12.3 f=456] text". diag_event_sync also forces the write-out + sceIoSync now. */
void diag_event(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void diag_event_sync(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* heap numbers (also used by the stat line) */
u32 diag_heap_used(void);
u32 diag_heap_peak(void);

/* GE: wait for the queue with a timeout (spin=1: busy poll for exact timings). On timeout the GE state (command
   registers of the draw in flight, call stack) goes into log.txt and the GE queues are reset (sceGeBreak(1)) so the
   game goes on: returns -1. diag_ge_state = that state line; returns sceGeDrawSync(1). */
int diag_ge_wait(const char *what, int timeout_ms, int spin);
int diag_ge_state(char *buf, int n);
u32 diag_ge_resets(void);

/* tests: kind 0 = real CPU exception (null store, hardware), 1 = software snapshot (emulator), 2 = hang (spin) */
void diag_test_crash(int kind);

#endif
