/*
 * ash_savetest: the save store on the PSP / PPSSPP (scripts/test-save-emu.sh). The step comes from "mode.txt" in
 * the working directory (host0:/ under PPSSPPHeadless --root):
 *   write   - two saves (sample 0 -> seq 1, sample 1 -> seq 2) through sceUtilitySavedata (A, then B), read back
 *   read    - fresh boot: read the newest copy, compare with sample <expect> (2nd word), check its seq (3rd word)
 *   file    - file backend only: write sample 2, read it back
 * Prints "ash-save: ..." lines and draws them on screen (screenshot).
 */
#include <pspkernel.h>
#include <pspdebug.h>
#include <pspdisplay.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include "savegame.h"
#include "savestore.h"
#include "sample.h"

PSP_MODULE_INFO("AshSaveTest", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);
PSP_HEAP_SIZE_KB(-1024);

static void say(const char *fmt, ...)
{
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    printf("ash-save: %s\n", line);
    pspDebugScreenPrintf("%s\n", line);
}

static SaveGame w, r, e;
static SaveStore st;

static void show_src(void)
{
    for (int i = 1; i < SV_SRC_COUNT; i++)
        say("  %-11s result %4d seq %u", sv_src_name(i), st.src_result[i], (unsigned)st.src_seq[i]);
}

/* compare r with sample <variant>, seq/time taken from r (they come from the store) */
static int check(int variant, unsigned want_seq)
{
    const char *what = 0;
    sample_game(&e, variant);
    e.seq = r.seq;
    e.time = r.time;
    int d = sg_compare(&e, &r, &what);
    int ok = d == 0 && (want_seq == 0 || r.seq == want_seq);
    say("compare with sample %d: %s%s, seq %u (want %u), name \"%s\" lvl %d souls %d items %d zones %d", variant,
        d == 0 ? "equal" : "DIFF ", d == 0 ? "" : what, (unsigned)r.seq, want_seq, r.name, r.level, (int)r.souls,
        r.n_item, r.n_zone);
    return ok;
}

static int timed_write(SaveGame *g)
{
    int frames = 0, res;
    if (sv_write_begin(&st, g) == SV_ERR) return SV_ERR;
    while ((res = sv_poll(&st)) == SV_BUSY) { sceDisplayWaitVblankStart(); frames++; }
    say("write sample: %s -> %s seq %u, %d frames, err 0x%08x", res == SV_OK ? "OK" : "ERR",
        sv_src_name(st.last_src), (unsigned)g->seq, frames, (unsigned)st.last_err);
    return res;
}

static int timed_read(void)
{
    int frames = 0, res;
    if (sv_read_begin(&st, &r) == SV_ERR) return SV_ERR;
    while ((res = sv_poll(&st)) == SV_BUSY) { sceDisplayWaitVblankStart(); frames++; }
    say("read: %s from %s, %d frames, err 0x%08x", res == SV_OK ? "OK" : "ERR", sv_src_name(st.last_src), frames,
        (unsigned)st.last_err);
    show_src();
    return res;
}

int main(void)
{
    pspDebugScreenInit();
    pspDebugScreenSetTextColor(0xffc0e0ff);
    char mode[64] = "write";
    int expect = 1;
    unsigned want_seq = 2;
    FILE *f = fopen("mode.txt", "r");
    if (f) {
        char m[32] = "";
        int a = 1;
        unsigned b = 2;
        int n = fscanf(f, "%31s %d %u", m, &a, &b);
        fclose(f);
        if (n >= 1) strcpy(mode, m);
        if (n >= 2) expect = a;
        if (n >= 3) want_seq = b;
    }
    say("Ash save test, mode %s", mode);
    int pass = 0;
    if (!strcmp(mode, "write")) {
        sv_init(&st, SV_BACKEND_ALL, 0);
        sample_game(&w, 0);
        int r1 = timed_write(&w);
        int s1 = st.last_src;
        sample_game(&w, 1);
        int r2 = timed_write(&w);
        int s2 = st.last_src;
        int rr = timed_read();
        pass = r1 == SV_OK && r2 == SV_OK && s1 == SV_SRC_UTIL_A && s2 == SV_SRC_UTIL_B && rr == SV_OK &&
               st.last_src == SV_SRC_UTIL_B && check(1, 2);
    } else if (!strcmp(mode, "read")) {
        sv_init(&st, SV_BACKEND_ALL, 0);
        pass = timed_read() == SV_OK && check(expect, want_seq);
    } else if (!strcmp(mode, "file")) {
        sv_init(&st, SV_BACKEND_FILE, 0);
        sample_game(&w, 2);
        int r1 = timed_write(&w);
        int rr = timed_read();
        pass = r1 == SV_OK && st.last_src == SV_SRC_FILE && rr == SV_OK && check(2, 0);
    }
    say("%s", pass ? "PASS" : "FAIL");
    for (int i = 0; i < 30; i++) sceDisplayWaitVblankStart();   /* keep the text on screen for the screenshot */
    sceKernelSleepThread();
    return 0;
}
