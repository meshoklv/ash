/*
 * Native (PC) run of the Ash "Asylum" simulation (src/game/asy_game.c + asy_auto.c) without a GPU: the same code
 * as the PSP EBOOT, the sceGu/sceIo calls of the modules are stubs (psp_stubs.c). Fast scenario checks:
 *
 *     tests/native/run.sh <scenario: route|hollow|death|boss> [max_seconds] [-v]
 *
 * Prints the game log and one summary line "ASY:{...}". Emulator/PSP behaviour (rendering, timing) is not covered.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "asy.h"

static int load(const char *name, u8 **out)
{
    FILE *f = fopen(name, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    u8 *b = NULL;
    if (posix_memalign((void **)&b, 64, (size_t)n + 64)) { fclose(f); return -1; }
    if (fread(b, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(b); return -1; }
    b[n] = 0;
    fclose(f);
    *out = b;
    return (int)n;
}

/* src/game/scene.c on the PSP */
int ash_load_file(const char *name, u8 **out, char *found, int found_len)
{
    int n = load(name, out);
    if (n >= 0 && found) snprintf(found, found_len, "%s", name);
    return n;
}
void ash_mat_mul(ScePspFMatrix4 *out, const ScePspFMatrix4 *a, const ScePspFMatrix4 *b)
{
    const float *A = (const float *)a, *B = (const float *)b;
    float r[16];
    for (int c = 0; c < 4; c++)
        for (int k = 0; k < 4; k++)
            r[c * 4 + k] = A[k] * B[c * 4] + A[4 + k] * B[c * 4 + 1] + A[8 + k] * B[c * 4 + 2] + A[12 + k] * B[c * 4 + 3];
    memcpy(out, r, 64);
}

static Game g;

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "asy_native <stage_dir> <scenario> [max_seconds] [-v]\n"); return 2; }
    if (chdir(argv[1])) { perror(argv[1]); return 2; }
    int sc = 0;
    for (int i = 1; i < ASY_SC_COUNT; i++) if (!strcmp(argv[2], auto_scenario_name(i))) sc = i;
    if (!sc) { fprintf(stderr, "unknown scenario %s\n", argv[2]); return 2; }
    float max_s = argc > 3 ? (float)atof(argv[3]) : 900;
    int every = 150, from = 0;
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "-v")) g.verbose = 1;
        else if (!strcmp(argv[i], "-vv")) g.verbose = 2;
        else if (!strcmp(argv[i], "-trace") && i + 2 < argc) { from = atoi(argv[i + 1]); every = atoi(argv[i + 2]); i += 2; }
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (game_load(&g, load)) { printf("load failed\n"); return 1; }
    game_start(&g, sc);
    RawPad pad = {0, 128, 128};
    int max_frames = (int)(max_s * 30);
    float end_wait = 0;
    while ((int)g.frame < max_frames) {
        game_step(&g, &pad);
        if (g.frame % (g.frame >= (unsigned)from ? every : 150) == 0)
            printf("ash-asy: f%u t=%.0fs prg=%d/%d st=%s pos=(%.2f %.2f %.2f) hp=%.0f/%.0f sp=%.0f souls=%d estus=%d step=%d lock=%d boss=%d/%d st%d\n",
                   (unsigned)g.frame, g.time, g.prg.phase, g.prg.sub, asy_pst_name[g.pl.st], g.pl.pos[0], g.pl.pos[1], g.pl.pos[2], g.pact.hp,
                   g.pact.hp_max, g.pact.sp, (int)g.prg.souls, (int)g.prg.estus, g.au.step, g.lock,
                   g.has_boss ? g.B.hp : 0, g.has_boss ? g.B.hp_max : 0, g.has_boss ? (int)g.B.st : -1);
        if (g.au.done) { end_wait += ASY_DT; if (end_wait > 1.0f) break; }
    }
    printf("ASY:{\"scenario\":\"%s\",\"frames\":%u,\"t\":%.1f,\"done\":%d,\"end_demo\":%d,\"victory\":%d,"
           "\"deaths\":%d,\"kills\":%d,\"reloads\":%d,\"saves\":%d,\"rests\":%d,\"bonfires\":%d,\"items\":%d,"
           "\"parries\":%d,\"ripostes\":%d,\"blocks\":%d,\"iframe_dodges\":%d,\"hits_given\":%d,\"hits_taken\":%d,"
           "\"plunges\":%d,\"estus\":%d,\"souls\":%d,\"warps\":%d,\"falls\":%d,\"boss_hp\":%d,\"step\":%d}\n",
           auto_scenario_name(sc), (unsigned)g.frame, g.time, g.au.done, g.end_demo, g.victory, g.deaths, g.kills,
           g.reloads, g.saves, g.rests, g.bonfires_lit, g.items, g.parries, g.ripostes, g.blocks, g.rolls_iframe,
           g.hits_given, g.hits_taken, g.plunges, g.estus_used, (int)g.prg.souls, g.au.warps, g.au.falls,
           g.has_boss ? g.B.hp : -1, g.au.step);
    return 0;
}
