/* Native unit test of src/save (scripts/test-save.sh): pack/unpack, damage detection, file store with seq, the
   progress bridge. Prints PASS n/n. Runs in its own directory (build/native/save). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "savegame.h"
#include "savestore.h"
#include "sample.h"
#include "save.h"

static int n_ok, n_all;
#define T(cond, ...) do { n_all++; if (cond) n_ok++; else { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static SaveGame a, b, c;
static unsigned char buf[SG_MAX_BYTES], buf2[SG_MAX_BYTES];

static void write_bytes(const char *p, const void *d, int n) { FILE *f = fopen(p, "wb"); fwrite(d, 1, n, f); fclose(f); }

int main(void)
{
    const char *what = 0;
    sample_game(&a, 0);
    int len = sg_pack(&a, buf, sizeof(buf));
    T(len > 0, "pack %d", len);
    printf("packed size: %d B (2 zones of events 1500 + 37 B)\n", len);
    T(sg_unpack(&b, buf, len) == 0, "unpack");
    T(sg_compare(&a, &b, &what) == 0, "roundtrip differs: %s", what ? what : "");
    T(strcmp(b.name, "Пепел") == 0, "name");
    T(sg_zone_get(&b, 10, 2) && sg_zone_get(&b, 10, 2)->size == 37, "zone 10_2");
    T(sg_zone_get(&b, 10, 1) == 0, "no zone 10_1");

    /* every single flipped bit in a sample of positions is caught (CRC or checks) and leaves b reset */
    int caught = 0, tried = 0;
    for (int i = 0; i < len; i += (i < 200 ? 1 : 37)) {
        memcpy(buf2, buf, len);
        buf2[i] ^= (unsigned char)(1u << (i % 8));
        tried++;
        if (sg_unpack(&b, buf2, len) != 0 && b.souls == 0 && b.n_item == 0) caught++;
    }
    T(caught == tried, "damage caught %d/%d", caught, tried);
    printf("damaged copies rejected: %d/%d\n", caught, tried);
    T(sg_unpack(&b, buf, len - 1) < 0, "truncated");
    T(sg_unpack(&b, buf, 10) < 0, "tiny");

    /* a newer format is refused, not misread: patch META.format and fix the CRC by repacking via save.h */
    {
        memcpy(buf2, buf, len);
        /* META is the first part: its data starts after the header (12 + 8 * n_parts) */
        int n_parts = buf2[8];
        int o = 12 + 8 * n_parts;
        buf2[o] = SG_FORMAT + 1;
        uint32_t crc = save_crc32(buf2, len - 4);
        buf2[len - 4] = crc; buf2[len - 3] = crc >> 8; buf2[len - 2] = crc >> 16; buf2[len - 1] = crc >> 24;
        T(sg_unpack(&b, buf2, len) == -5, "newer format -> -5");
    }

    /* bad fields with a valid CRC: equip index out of range */
    {
        sample_game(&c, 0);
        c.equip[3] = 40;   /* > n_item */
        int l2 = sg_pack(&c, buf2, sizeof(buf2));
        T(l2 > 0 && sg_unpack(&b, buf2, l2) == -6, "bad equip index -> -6");
    }

    /* file store: write twice, seq grows, the newest is read back */
    remove("t.sav"); remove("t.sav.tmp");
    SaveStore st;
    sv_init(&st, SV_BACKEND_ALL, "t.sav");
    T(sv_read_sync(&st, &b) == SV_ERR && st.newest_src == SV_SRC_NONE, "empty store -> no save");
    sample_game(&a, 0);
    T(sv_write_sync(&st, &a) == SV_OK && a.seq == 1, "write 1 seq %u", (unsigned)a.seq);
    sample_game(&a, 1);
    T(sv_write_sync(&st, &a) == SV_OK && a.seq == 2, "write 2 seq %u", (unsigned)a.seq);
    SaveStore st2;
    sv_init(&st2, SV_BACKEND_ALL, "t.sav");   /* "restart" */
    T(sv_read_sync(&st2, &b) == SV_OK && st2.last_src == SV_SRC_FILE, "read back");
    T(b.seq == 2 && sg_compare(&a, &b, &what) == 0, "read == written: %s", what ? what : "");
    /* write without a read first: seq still continues from the file */
    SaveStore st3;
    sv_init(&st3, SV_BACKEND_ALL, "t.sav");
    sample_game(&c, 2);
    T(sv_write_sync(&st3, &c) == SV_OK && c.seq == 3, "write w/o read seq %u", (unsigned)c.seq);
    /* a cut between remove and rename leaves only t.sav.tmp: still loads */
    {
        FILE *f = fopen("t.sav", "rb");
        int l = (int)fread(buf2, 1, sizeof(buf2), f);
        fclose(f);
        remove("t.sav");
        write_bytes("t.sav.tmp", buf2, l);
        sv_init(&st2, SV_BACKEND_ALL, "t.sav");
        T(sv_read_sync(&st2, &b) == SV_OK && b.seq == 3, "tmp fallback seq %u", (unsigned)b.seq);
        remove("t.sav.tmp");
    }
    /* damaged file: no valid save, target untouched */
    {
        len = sg_pack(&c, buf2, sizeof(buf2));
        buf2[len / 2] ^= 0x10;
        write_bytes("t.sav", buf2, len);
        sv_init(&st2, SV_BACKEND_ALL, "t.sav");
        strcpy(b.name, "untouched");
        int r = sv_read_sync(&st2, &b);
        T(r == SV_ERR && st2.src_result[SV_SRC_FILE] == -3 && strcmp(b.name, "untouched") == 0,
          "damaged file r=%d res=%d", r, st2.src_result[SV_SRC_FILE]);
        remove("t.sav");
    }

    /* progress bridge */
    {
        Progress p;
        float hp = 300, hp_max = 572;
        prg_init(&p, 0);
        prg_bind_hp(&p, &hp, &hp_max);
        p.souls = 777; p.estus = 2; p.deaths = 1; p.flags = 9;
        sg_init(&c);
        sg_from_progress(&c, &p);
        T(c.souls == 777 && c.estus == 2 && c.hp == 300 && c.hp_max == 572 && c.deaths == 1 && c.prg_flags == 9,
          "from_progress souls %d estus %d hp %g", (int)c.souls, (int)c.estus, c.hp);
        /* dying: the save holds what the reload makes (souls -> bloodstain) */
        float pos[3] = {1, 2, 3};
        prg_player_died(&p, pos);
        for (int i = 0; i < 200 && !(p.phase == PRG_DYING && p.sub == 1) && !(p.phase == PRG_LOADING); i++) {
            PrgInput in;
            memset(&in, 0, sizeof(in));
            prg_tick(&p, &in, 1.0f / 30);
        }
        sg_from_progress(&c, &p);
        T(c.souls == 0 && c.stain_on && c.stain_souls == 777 && c.deaths == 2,
          "dying save souls %d stain %d/%d deaths %d", (int)c.souls, (int)c.stain_on, (int)c.stain_souls,
          (int)c.deaths);
        Progress q;
        float hq = 1, hq_max = 600;
        prg_init(&q, 0);
        prg_bind_hp(&q, &hq, &hq_max);
        sample_game(&c, 0);
        sg_to_progress(&c, &q);
        T(q.souls == 1234 && q.estus == 3 && hq == 512.5f && q.stain_on && q.stain_souls == 60, "to_progress");
    }

    printf("%s %d/%d\n", n_ok == n_all ? "PASS" : "FAIL", n_ok, n_all);
    return n_ok == n_all ? 0 : 1;
}
