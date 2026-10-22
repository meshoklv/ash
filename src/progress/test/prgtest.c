/*
 * Native unit test of src/progress (no PSP): Estus (charges, clip sequence, heal amount and clamp, empty flask,
 * interrupt), death -> banner -> fade -> reload -> bloodstain -> pickup, second death loses the old stain, bonfire
 * rest (sit, menu skips the disabled item, stand), souls autosave throttle, save blob + container round trip,
 * CRC catches a flipped byte. Build + run: scripts/test-progress.sh. Prints "PASS n/n" or the failed checks.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "../progress.h"
#include "../save.h"
#include "uistr.h"

static int g_n, g_fail;
#define CHECK(c, ...) do { g_n++; if (!(c)) { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static const float DT = 1.0f / 30.0f;

typedef struct { int anims[64]; int n_anim; uint32_t req_all; int reloads, saves; } Log;

static void run(Progress *p, PrgInput *in, float seconds, Log *lg)
{
    int steps = (int)(seconds * 30.0f + 0.5f);
    for (int i = 0; i < steps; i++) {
        prg_tick(p, in, DT);
        in->item = in->up = in->down = in->confirm = in->cancel = 0;   /* edges */
        lg->req_all |= p->req;
        if ((p->req & PRG_REQ_ANIM) && lg->n_anim < 64) lg->anims[lg->n_anim++] = p->anim;
        if (p->req & PRG_REQ_RELOAD) { lg->reloads++; prg_on_reload(p); }  /* what EV_CMD_RELOAD would do */
        if (p->req & PRG_REQ_SAVE) lg->saves++;
    }
}

int main(void)
{
    float hp = 572, hp_max = 572;   /* the player's CbActor */
    Progress p;
    prg_init(&p, 0);
    prg_bind_hp(&p, &hp, &hp_max);
    PrgInput in;
    memset(&in, 0, sizeof(in));
    Log lg;
    memset(&lg, 0, sizeof(lg));

    /* ---- Estus */
    CHECK(p.estus == 5, "estus start %d", p.estus);
    hp = 100;
    in.item = 1;
    run(&p, &in, 0.05f, &lg);
    CHECK(p.phase == PRG_DRINK && p.estus == 4, "drink started phase %d estus %d", p.phase, p.estus);
    CHECK(prg_locked(&p), "locked while drinking");
    run(&p, &in, 4.0f, &lg);
    CHECK(p.phase == PRG_PLAY, "drink over, phase %d", p.phase);
    CHECK(fabsf(hp - 400) < 0.5f, "healed +300 -> %.1f", hp);
    CHECK(lg.n_anim == 3 && lg.anims[0] == PRG_ANIM_DRINK_IN && lg.anims[1] == PRG_ANIM_DRINK &&
          lg.anims[2] == PRG_ANIM_DRINK_OUT, "clip order n=%d %d %d %d", lg.n_anim, lg.anims[0], lg.anims[1], lg.anims[2]);
    hp = 500;
    in.item = 1;
    run(&p, &in, 4.0f, &lg);
    CHECK(hp == hp_max, "heal clamps at max %.1f", hp);
    in.item = 1; in.busy = 1;
    run(&p, &in, 0.1f, &lg);
    CHECK(p.phase == PRG_PLAY && p.estus == 3, "busy blocks the item (estus %d)", p.estus);
    in.busy = 0;
    /* interrupt before the heal: charge spent, no HP */
    hp = 100;
    in.item = 1;
    run(&p, &in, 0.3f, &lg);
    prg_interrupt(&p);
    run(&p, &in, 2.0f, &lg);
    CHECK(p.estus == 2 && hp == 100 && p.phase == PRG_PLAY, "interrupt: estus %d hp %.0f", p.estus, hp);
    p.estus = 0;
    lg.n_anim = 0;
    in.item = 1;
    run(&p, &in, 3.0f, &lg);
    CHECK(lg.n_anim == 1 && lg.anims[0] == PRG_ANIM_DRINK_EMPTY && hp == 100, "empty flask anim %d hp %.0f",
          lg.anims[0], hp);

    /* ---- souls + death */
    prg_add_souls(&p, 20);
    prg_add_souls(&p, 20);
    CHECK(p.souls == 40 && p.souls_gain == 40, "souls %d gain %d", p.souls, p.souls_gain);
    float dpos[3] = {-3.0f, 182.0f, -20.0f};
    lg.n_anim = 0; lg.reloads = 0;
    prg_player_died(&p, dpos);
    run(&p, &in, DT, &lg);
    CHECK(lg.n_anim == 1 && lg.anims[0] == PRG_ANIM_DEATH && prg_locked(&p), "death anim requested");
    run(&p, &in, 2.0f, &lg);
    CHECK(p.banner == IMG_DIED, "banner ВЫ ПОГИБЛИ at 2 s (banner %d)", p.banner);
    run(&p, &in, 3.5f, &lg);
    CHECK(p.fade > 0.4f && lg.reloads == 0, "fading to black %.2f", p.fade);
    run(&p, &in, 0.6f, &lg);
    CHECK(lg.reloads == 1 && p.loading == 1, "reload requested once (%d), loading %d", lg.reloads, p.loading);
    CHECK(p.souls == 0 && p.stain_on && p.stain_souls == 40 && p.deaths == 1, "stain %d souls %d", p.stain_souls,
          p.souls);
    CHECK(hp == hp_max && p.estus == 5, "refill after death hp %.0f estus %d", hp, p.estus);
    run(&p, &in, 3.0f, &lg);
    CHECK(p.phase == PRG_PLAY && p.fade == 0 && p.banner < 0, "back to play (phase %d fade %.2f)", p.phase, p.fade);
    /* walk onto the stain */
    in.pos[0] = -3.5f; in.pos[1] = 182.0f; in.pos[2] = -20.3f;
    run(&p, &in, 0.1f, &lg);
    CHECK(p.souls == 40 && !p.stain_on && (lg.req_all & PRG_REQ_SOULS_BACK), "picked up: souls %d", p.souls);
    /* die twice: the first stain is lost */
    in.pos[0] = 0; in.pos[2] = 0;
    prg_player_died(&p, dpos);
    run(&p, &in, 9.0f, &lg);
    prg_add_souls(&p, 100);
    float d2[3] = {10, 182, 10};
    prg_player_died(&p, d2);
    run(&p, &in, 9.0f, &lg);
    CHECK(p.stain_on && p.stain_souls == 100 && p.stain_pos[0] == 10 && p.deaths == 3,
          "second death: old 40 lost, new stain %d at x=%.0f", p.stain_souls, p.stain_pos[0]);

    /* ---- bonfire rest */
    lg.n_anim = 0;
    hp = 50;
    prg_rest_begin(&p);
    prg_on_reload(&p);   /* EV_CMD_RELOAD after EV_CMD_REST */
    CHECK(hp == hp_max && p.estus == 5 && prg_resting(&p), "rest refill");
    run(&p, &in, 2.5f, &lg);
    CHECK(p.sub == 1 && p.menu_cursor == 1, "menu up, cursor on Leave (%d)", p.menu_cursor);
    in.up = 1;
    run(&p, &in, 0.1f, &lg);
    CHECK(p.menu_cursor == 1, "disabled Level Up skipped (%d)", p.menu_cursor);
    in.confirm = 1;
    run(&p, &in, 4.0f, &lg);
    CHECK(p.phase == PRG_PLAY && (lg.req_all & PRG_REQ_STAND_DONE), "stood up");
    CHECK(lg.n_anim == 3 && lg.anims[0] == PRG_ANIM_SIT && lg.anims[1] == PRG_ANIM_SIT_LOOP &&
          lg.anims[2] == PRG_ANIM_STAND, "rest clips %d %d %d", lg.anims[0], lg.anims[1], lg.anims[2]);

    /* ---- autosave throttle on souls */
    lg.saves = 0;
    p.since_save = 0;   /* as if just saved */
    prg_add_souls(&p, 5);
    prg_add_souls(&p, 7);
    run(&p, &in, 5.0f, &lg);
    CHECK(lg.saves == 0, "no save before 10 s (%d)", lg.saves);
    run(&p, &in, 6.0f, &lg);
    CHECK(lg.saves == 1, "one save after 10 s (%d)", lg.saves);

    /* ---- banners */
    prg_banner(&p, IMG_VICTORY);
    run(&p, &in, 1.0f, &lg);
    CHECK(p.banner == IMG_VICTORY, "victory banner on");
    run(&p, &in, 4.0f, &lg);
    CHECK(p.banner < 0, "victory banner off after ~4.2 s");

    /* ---- save round trip */
    uint8_t blob[256], file[1024];
    int n = prg_save_write(&p, blob, sizeof(blob));
    CHECK(n == prg_save_size(), "blob size %d", n);
    const char evblob[] = "event-flags";
    SavePart parts[2] = {{SAVE_TAG_PROGRESS, blob, n}, {SAVE_TAG_EVENTS, evblob, (int)sizeof(evblob)}};
    int len = save_pack(file, sizeof(file), parts, 2);
    CHECK(len == save_packed_size(parts, 2), "packed %d", len);
    SavePart got[4];
    int np = save_unpack(file, len, got, 4);
    CHECK(np == 2, "unpack parts %d", np);
    const SavePart *pp = save_find(got, np, SAVE_TAG_PROGRESS), *pe = save_find(got, np, SAVE_TAG_EVENTS);
    CHECK(pp && pe && pe->size == (int)sizeof(evblob) && !memcmp(pe->data, evblob, sizeof(evblob)), "parts found");
    Progress q;
    float qhp = 1, qmax = 572;
    prg_init(&q, 0);
    prg_bind_hp(&q, &qhp, &qmax);
    CHECK(pp && prg_save_read(&q, pp->data, pp->size) == 0, "blob read");
    CHECK(q.souls == p.souls && q.stain_on == p.stain_on && q.stain_souls == p.stain_souls && q.deaths == p.deaths &&
          q.estus == p.estus && qhp == hp, "fields equal (souls %d/%d)", q.souls, p.souls);
    file[20] ^= 0x40;
    CHECK(save_unpack(file, len, got, 4) == -3, "CRC catches a flipped byte");
    /* file I/O (stdio branch) */
    CHECK(save_write_file("prgtest.sav", parts, 2) == 0, "write file");
    uint8_t rb[1024];
    CHECK(save_read_file("prgtest.sav", rb, sizeof(rb), got, 4) == 2, "read file");
    remove("prgtest.sav");
    CHECK(save_read_file("prgtest.sav", rb, sizeof(rb), got, 4) == -10, "missing file -> -10");
    /* a save written while dying stores the death already */
    prg_add_souls(&p, 77);
    int souls_before = p.souls;
    prg_player_died(&p, dpos);
    prg_save_write(&p, blob, sizeof(blob));
    prg_init(&q, 0);
    prg_bind_hp(&q, &qhp, &qmax);
    prg_save_read(&q, blob, n);
    CHECK(q.souls == 0 && q.stain_souls == souls_before && q.stain_on, "save while dying: stain %d", q.stain_souls);

    printf("%s %d/%d\n", g_fail ? "FAIL" : "PASS", g_n - g_fail, g_n);
    return g_fail ? 1 : 0;
}
