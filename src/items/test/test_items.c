/* Native unit tests of src/items on the real cooked data (items.ashit from tools/cook_items.py).
 *   src/items/test/run.sh [path/to/items.ashit]
 * Prints "TEST:{...}" with the counts at the end; exit code = failures. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "items.h"

static int checks, fails;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
#define NEAR(a, b, eps) (fabsf((float)(a) - (float)(b)) <= (eps))

#define W(id) ITM_KEY(ITM_CAT_WEAPON, id)
#define A(id) ITM_KEY(ITM_CAT_ARMOR, id)
#define R(id) ITM_KEY(ITM_CAT_RING, id)
#define G(id) ITM_KEY(ITM_CAT_GOODS, id)

static const uint8_t WARRIOR[8] = {11, 8, 12, 13, 13, 11, 9, 9};   /* VIT ATT END STR DEX RES INT FTH (PARAM 3000) */

static uint8_t *load(const char *path, int *n)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    *n = (int)ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)*n);
    if (fread(b, 1, (size_t)*n, f) != (size_t)*n) { fclose(f); free(b); return 0; }
    fclose(f);
    return b;
}

static int utf8_ok(const char *s)
{
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        int n = *p < 0x80 ? 0 : (*p & 0xe0) == 0xc0 ? 1 : (*p & 0xf0) == 0xe0 ? 2 : (*p & 0xf8) == 0xf0 ? 3 : -1;
        if (n < 0) return 0;
        p++;
        for (int i = 0; i < n; i++, p++) if ((*p & 0xc0) != 0x80) return 0;
    }
    return 1;
}

static void dump_inv(const Inv *inv, const ItmDb *db, const char *title)
{
    printf("-- %s: %d entries\n", title, inv->n);
    for (int i = 0; i < inv->n; i++) {
        int s = itm_slot_of(inv, i);
        printf("   %2d %-40s x%-3d %s%d\n", i, itm_name(db, inv->e[i].key), inv->e[i].count, s >= 0 ? "slot " : "", s);
    }
}

int main(int argc, char **argv)
{
    char def[512];
    snprintf(def, sizeof def, "%s/ash-assets/cooked/items/items.ashit", getenv("HOME") ? getenv("HOME") : ".");
    const char *path = argc > 1 ? argv[1] : def;
    int size = 0;
    uint8_t *file = load(path, &size);
    if (!file) { printf("no %s\n", path); return 1; }
    ItmDb db;
    int rc = itm_db_load(&db, file, size);
    CHECK(rc == 0, "db load %d", rc);
    if (rc) return 1;
    printf("db: %u items, %u lots, %u classes, %u graphs, %d B\n", db.h->n_item, db.h->n_lot, db.h->n_class,
           db.h->n_graph, size);
    /* corrupted file is refused */
    {
        uint8_t *bad = malloc((size_t)size);
        memcpy(bad, file, (size_t)size);
        ((ItmHdr *)bad)->n_item = 1000000;
        ItmDb b2;
        CHECK(itm_db_load(&b2, bad, size) < 0, "corrupt n_item accepted");
        CHECK(itm_db_load(&b2, bad, 40) < 0, "short file accepted");
        free(bad);
    }

    /* ---- database: names from FMG RUSSIAN, numbers from PARAM */
    CHECK(strcmp(itm_name(&db, W(201000)), "Длинный меч") == 0, "longsword name '%s'", itm_name(&db, W(201000)));
    CHECK(strcmp(itm_name(&db, G(2010)), "Ключ от темницы в подземелье") == 0, "cell key '%s'", itm_name(&db, G(2010)));
    CHECK(itm_find(&db, W(999999)) == 0, "unknown key found");
    const ItmRec *ls = itm_find(&db, W(201000));
    CHECK(ls && ls->v[ITM_WV_ATK] == 80 && ls->kind == ITM_WK_MELEE && (ls->flags & ITM_F_RIGHT), "longsword rec");
    CHECK(ls && strlen(itm_str(&db, ls->desc)) > 20, "longsword caption");
    const ItmRec *est = itm_find(&db, G(201));
    CHECK(est && est->use == ITM_USE_ESTUS && est->use_val == 3020, "estus rec");

    /* ---- new game: full kit (Warrior) -> equipment weight = the menu's class weight 21.4 (tools/cook_menu.py) */
    Inv inv;
    int32_t estus = 5;
    itm_new_game(&inv, &db, 3000, 0, 0);
    inv.estus = &estus;
    dump_inv(&inv, &db, "Warrior, full start");
    CHECK(itm_equipped_key(&inv, ITM_SLOT_R1) == W(201000), "warrior R1 longsword");
    CHECK(itm_equipped_key(&inv, ITM_SLOT_L1) == W(1450000), "warrior L1 heater shield");
    CHECK(itm_equipped_key(&inv, ITM_SLOT_HEAD) == A(210000), "warrior head");
    CHECK(itm_equipped_key(&inv, ITM_SLOT_Q1) == G(201), "estus in Q1");
    CHECK(itm_has(&inv, G(117)) && itm_has(&inv, G(103)), "darksign + black separation crystal");
    ItmStats st;
    itm_calc(&inv, &db, WARRIOR, &st);
    printf("warrior: hp %.0f sp %.0f load %.1f/%.1f (%.0f%% roll %d) poise %.0f def %.0f/%.0f/%.0f/%.0f AR R %.1f L %.1f\n",
           st.hp_max, st.sp_max, st.load, st.load_max, st.load_ratio * 100, st.roll, st.poise, st.def[0], st.def[1],
           st.def[2], st.def[3], st.ar_r[0], st.ar_l[0]);
    CHECK(NEAR(st.load, 21.4f, 0.05f), "warrior load %.2f != 21.4", st.load);
    CHECK(NEAR(st.load_max, 52.0f, 0.01f), "load max 40+END");
    CHECK(st.roll == ITM_ROLL_MID, "warrior mid roll (%.0f%%)", st.load_ratio * 100);
    CHECK(st.guard_l[0] == 100 && st.req_ok_r && st.req_ok_l, "heater shield 100%% phys");

    /* ---- Knight full: weight 31.0; longsword AR with the Knight's STR/DEX 11 = 86.94 (tools/cook_combat.py) */
    const ItmClass *kn = itm_find_class(&db, 3001);
    CHECK(kn && kn->attr[0] == 14 && kn->level == 5, "knight class row");
    itm_new_game(&inv, &db, 3001, 2405, 0);
    itm_calc(&inv, &db, kn->attr, &st);
    CHECK(NEAR(st.load, 31.0f, 0.05f), "knight load %.2f != 31.0", st.load);
    CHECK(NEAR(st.hp_max, 572, 0.5f), "knight hp %.0f", st.hp_max);
    CHECK(NEAR(st.sp_max, 91, 0.5f), "knight stamina %.0f", st.sp_max);
    {
        float ar[4];
        int ok;
        itm_weapon_ar(&db, ls, kn->attr, 0, ar, &ok);
        printf("longsword AR with knight stats %.2f (cook_combat 86.94)\n", ar[0]);
        CHECK(NEAR(ar[0], 86.94f, 0.05f) && ok, "longsword AR %.2f", ar[0]);
        float ar2[4];
        itm_weapon_ar(&db, ls, kn->attr, 1, ar2, &ok);
        CHECK(ar2[0] > ar[0], "two-handed AR %.2f > %.2f", ar2[0], ar[0]);
    }

    /* ---- Asylum start (Knight): hilt in R1, no shield, no Estus, class armour */
    itm_new_game(&inv, &db, 3001, 0, 1);
    dump_inv(&inv, &db, "Knight, Asylum start");
    CHECK(itm_equipped_key(&inv, ITM_SLOT_R1) == W(212000), "hilt in R1");
    CHECK(itm_equipped(&inv, ITM_SLOT_L1) < 0, "no shield");
    CHECK(!itm_has(&inv, G(201)) && !itm_has(&inv, G(200)), "no estus");
    CHECK(!itm_has(&inv, W(202000)), "no broadsword yet");
    CHECK(itm_equipped_key(&inv, ITM_SLOT_BODY) == A(391000), "knight armour");

    /* ---- loot of m18_01 in route order */
    uint32_t rng = 12345;
    ItmGot got[8];
    char txt[256];
    int n = itm_award_lot(&inv, &db, 1810000, &rng, got, 8);
    itm_got_text(&db, got, n, txt, sizeof txt);
    printf("lot 1810000 -> %s\n", txt);
    CHECK(n == 1 && itm_has(&inv, G(2010)), "cell key");
    CHECK(strcmp(txt, "Получено: Ключ от темницы в подземелье") == 0, "got text '%s'", txt);
    itm_used_text(&db, G(2010), txt, sizeof txt);
    CHECK(strcmp(txt, "Использовано: Ключ от темницы в подземелье") == 0, "used text '%s'", txt);
    ItmUse use;
    CHECK(itm_use(&inv, &db, itm_find_entry(&inv, G(2010)), &use) == ITM_ERR_USE, "a key is not used from the menu");
    /* class weapons at bonfire 2 (Knight: broadsword 1810120, tower shield 1810130) */
    itm_award_lot(&inv, &db, 1810120, &rng, got, 8);
    itm_award_lot(&inv, &db, 1810130, &rng, got, 8);
    int bs = itm_find_entry(&inv, W(202000)), ts = itm_find_entry(&inv, W(1452000));
    itm_calc(&inv, &db, kn->attr, &st);
    float load0 = st.load, ar0 = st.ar_r[0];
    CHECK(itm_equip(&inv, &db, ITM_SLOT_R1, bs) == ITM_OK, "equip broadsword");
    CHECK(itm_equip(&inv, &db, ITM_SLOT_L1, ts) == ITM_OK, "equip tower shield");
    CHECK(itm_slot_of(&inv, itm_find_entry(&inv, W(212000))) < 0, "hilt unequipped");
    itm_calc(&inv, &db, kn->attr, &st);
    printf("broadsword + tower shield: load %.1f -> %.1f, AR %.1f -> %.1f, guard %.0f%%\n", load0, st.load, ar0,
           st.ar_r[0], st.guard_l[0]);
    CHECK(NEAR(st.load, load0 - 1.0f + 3.0f + 3.0f, 0.01f) && NEAR(st.load, 31.0f, 0.05f) && st.ar_r[0] > ar0,
          "load / AR recomputed: %.2f", st.load);
    /* wrong slots */
    CHECK(itm_equip(&inv, &db, ITM_SLOT_HEAD, ts) == ITM_ERR_SLOT, "shield on the head");
    CHECK(itm_equip(&inv, &db, ITM_SLOT_Q1, bs) == ITM_ERR_SLOT, "sword as quick item");
    /* move R1 weapon to L1: L1's old item (tower shield) goes back to R1 only if it fits the right hand */
    CHECK(itm_equip(&inv, &db, ITM_SLOT_L1, bs) == ITM_OK, "broadsword to the left hand");
    CHECK(itm_equipped(&inv, ITM_SLOT_L1) == bs && itm_equipped(&inv, ITM_SLOT_R1) != bs, "moved, not duplicated");
    itm_equip(&inv, &db, ITM_SLOT_R1, bs);
    itm_equip(&inv, &db, ITM_SLOT_L1, ts);
    /* R1/R2 switch */
    int hilt = itm_find_entry(&inv, W(212000));
    CHECK(itm_equip(&inv, &db, ITM_SLOT_R2, hilt) == ITM_OK, "hilt to R2");
    itm_switch_right(&inv);
    CHECK(itm_right_weapon(&inv) == W(212000), "switch to R2");
    itm_switch_right(&inv);
    CHECK(itm_right_weapon(&inv) == W(202000), "switch back");

    /* rings: rusted iron ring; the same ring twice is refused */
    itm_award_lot(&inv, &db, 1810060, &rng, got, 8);
    itm_add(&inv, &db, R(125), 1);
    int r1 = itm_find_entry(&inv, R(125)), r2 = -1;
    for (int i = inv.n - 1; i >= 0; i--) if (inv.e[i].key == R(125)) { r2 = i; break; }
    CHECK(r1 >= 0 && r2 > r1, "two rusted rings are two entries");
    CHECK(itm_equip(&inv, &db, ITM_SLOT_RING1, r1) == ITM_OK, "ring 1");
    CHECK(itm_equip(&inv, &db, ITM_SLOT_RING2, r2) == ITM_ERR_SLOT, "same ring twice refused");
    /* Havel's ring (gift-less test via add): load x1.5 */
    if (itm_find(&db, R(100))) {
        itm_add(&inv, &db, R(100), 1);
        float lm = (itm_calc(&inv, &db, kn->attr, &st), st.load_max);
        itm_equip(&inv, &db, ITM_SLOT_RING2, itm_find_entry(&inv, R(100)));
        itm_calc(&inv, &db, kn->attr, &st);
        CHECK(NEAR(st.load_max, lm * 1.5f, 0.01f), "havel x1.5 %.1f", st.load_max);
        itm_unequip(&inv, ITM_SLOT_RING2);
    } else {
        printf("note: Havel's ring not in this item set (not reachable in m18_01/m10_02) - skipped\n");
    }

    /* soul item: +200 souls, consumed */
    itm_award_lot(&inv, &db, 1810070, &rng, got, 8);
    int si = itm_find_entry(&inv, G(400));
    CHECK(itm_use(&inv, &db, si, &use) == ITM_OK && use.kind == ITM_USE_SOULS && use.value == 200 && use.consumed == 1,
          "soul of a lost undead: kind %d value %d consumed %d", use.kind, use.value, use.consumed);
    CHECK(!itm_has(&inv, G(400)), "soul item used up");

    /* Firelink loot: humanity x3, fire bombs x6 */
    n = itm_award_lot(&inv, &db, 1020000, &rng, got, 8);
    itm_got_text(&db, got, n, txt, sizeof txt);
    printf("lot 1020000 -> %s\n", txt);
    CHECK(strcmp(txt, "Получено: Человечность x3") == 0, "humanity text '%s'", txt);
    int h0 = inv.humanity;
    CHECK(itm_use(&inv, &db, itm_find_entry(&inv, G(500)), &use) == ITM_OK && use.kind == ITM_USE_HUMANITY &&
          inv.humanity == h0 + 1 && itm_count(&inv, G(500)) == 2, "humanity +1");
    if (itm_add(&inv, &db, G(501), 1) == 1) {
        itm_use(&inv, &db, itm_find_entry(&inv, G(501)), &use);
        CHECK(inv.humanity == h0 + 3, "twin humanity +2 -> %d", inv.humanity);
    }
    itm_award_lot(&inv, &db, 1020110, &rng, got, 8);
    int fb = itm_find_entry(&inv, G(292));
    CHECK(itm_count(&inv, G(292)) == 6, "6 fire bombs");
    CHECK(itm_equip(&inv, &db, ITM_SLOT_Q2, fb) == ITM_OK, "bomb to Q2");
    inv.quick = 0;
    CHECK(itm_quick_next(&inv) == ITM_SLOT_Q2, "quick cycles to the bomb (Q1 empty in the Asylum start)");
    CHECK(itm_use_quick(&inv, &db, &use) == ITM_OK && use.kind == ITM_USE_THROW && use.value > 0, "throw bullet %d", use.value);
    CHECK(itm_count(&inv, G(292)) == 5, "5 bombs left");
    for (int k = 0; k < 5; k++) itm_use_quick(&inv, &db, &use);
    CHECK(!itm_has(&inv, G(292)) && itm_equipped(&inv, ITM_SLOT_Q2) < 0, "bombs used up, slot empty");

    /* Estus from Oscar: a flask, charges belong to progress */
    n = itm_award_lot(&inv, &db, 1080, &rng, got, 8);
    itm_add(&inv, &db, G(200), 1);
    CHECK(itm_add(&inv, &db, G(201), 1) == 0, "only one flask");
    int fl = itm_find_entry(&inv, G(200));
    CHECK(fl >= 0 && itm_equip(&inv, &db, ITM_SLOT_Q1, fl) == ITM_OK, "flask to Q1");
    CHECK(itm_use(&inv, &db, fl, &use) == ITM_OK && use.kind == ITM_USE_ESTUS && !use.consumed && itm_has(&inv, G(200)),
          "estus is not removed");

    /* stack limit: humanity max 99 */
    int had = itm_count(&inv, G(500));
    int add = itm_add(&inv, &db, G(500), 200);
    CHECK(itm_count(&inv, G(500)) == 99 && add == 99 - had, "stack capped at 99 (added %d)", add);
    ItmGot full = {G(500), 1, itm_add(&inv, &db, G(500), 1)};
    itm_got_text(&db, &full, 1, txt, sizeof txt);
    CHECK(strcmp(txt, "Нельзя унести больше") == 0, "full text '%s'", txt);

    /* boss drop: the big pilot key opens the door (src/event `need` key) */
    itm_award_lot(&inv, &db, 2660, &rng, got, 8);
    CHECK(itm_has(&inv, G(2011)), "big pilot key");

    /* remove an equipped item: slot cleared, other slots keep their items (indices shift) */
    uint32_t body = itm_equipped_key(&inv, ITM_SLOT_BODY);
    int hi = itm_find_entry(&inv, A(390000));
    CHECK(itm_remove_at(&inv, hi, 1) == 1, "remove helmet");
    CHECK(itm_equipped(&inv, ITM_SLOT_HEAD) < 0 && itm_equipped_key(&inv, ITM_SLOT_BODY) == body &&
          itm_right_weapon(&inv) == W(202000), "slots after removal");

    /* requirement penalty: the Demon's Great Hammer (STR 46) with the Knight */
    itm_award_lot(&inv, &db, 2661, &rng, got, 8);
    {
        float ar[4];
        int ok = 1;
        const ItmRec *hm = itm_find(&db, W(852000));
        itm_weapon_ar(&db, hm, kn->attr, 1, ar, &ok);
        CHECK(hm && !ok && NEAR(ar[0], hm->v[ITM_WV_ATK] * 0.6f, 0.01f), "hammer penalty");
    }
    /* overload: everything heavy on */
    itm_equip(&inv, &db, ITM_SLOT_R2, itm_find_entry(&inv, W(852000)));
    itm_calc(&inv, &db, kn->attr, &st);
    printf("with the hammer: load %.1f/%.1f roll %d\n", st.load, st.load_max, st.roll);
    CHECK(st.roll == ITM_ROLL_FAT && NEAR(st.load, 31.0f - 4.2f + 22.0f, 0.05f), "fat roll with the hammer in R2");
    {
        uint8_t weak[8];
        memcpy(weak, kn->attr, 8);
        weak[2] = 5;                                   /* END 5 -> load max 45 */
        itm_calc(&inv, &db, weak, &st);
        CHECK(st.roll == ITM_ROLL_OVER && NEAR(st.load_max, 45, 0.01f), "overloaded with END 5");
    }

    /* long pickup text: whole names only, valid UTF-8 */
    {
        ItmGot many[12];
        for (int i = 0; i < 12; i++) { many[i].key = W(201000); many[i].count = many[i].added = 1; }
        char big[512];
        itm_got_text(&db, many, 12, big, sizeof big);
        printf("long text (%d B): %s\n", (int)strlen(big), big);
        CHECK(utf8_ok(big) && strstr(big, "\xe2\x80\xa6") != 0, "long got text cut at an item");
        for (int cap = 20; cap < 48; cap++) {      /* every cut position: never half a character */
            char small[48];
            int k = itm_got_text(&db, many, 2, small, cap);
            CHECK(utf8_ok(small) && k == (int)strlen(small) && k < cap, "cap %d cut '%s'", cap, small);
            if (cap >= 44) CHECK(strstr(small, "меч") != 0, "cap %d: whole first name '%s'", cap, small);
        }
    }

    /* save round trip */
    {
        uint8_t buf[1024];
        int len = itm_save_write(&inv, buf, sizeof buf);
        CHECK(len == itm_save_size(), "save size %d", len);
        Inv back;
        itm_inv_init(&back);
        CHECK(itm_save_read(&back, &db, buf, len) == 0, "save read");
        int same = back.n == inv.n && memcmp(back.eq, inv.eq, sizeof inv.eq) == 0 && back.humanity == inv.humanity;
        for (int i = 0; same && i < inv.n; i++) same = back.e[i].key == inv.e[i].key && back.e[i].count == inv.e[i].count;
        CHECK(same, "save round trip");
        buf[0] ^= 1;
        CHECK(itm_save_read(&back, &db, buf, len) < 0, "bad magic refused");
    }

    /* inventory full */
    {
        Inv t;
        itm_inv_init(&t);
        int k = 0;
        for (int i = 0; i < 100; i++) k += itm_add(&t, &db, W(201000), 1);
        CHECK(k == ITM_INV_MAX && t.n == ITM_INV_MAX, "inventory limit %d", k);
    }

    /* enemy drop: weighted pick (hollow c2500 lot 25002200: 98 nothing / 2 broken straight sword) */
    {
        int hits = 0, other = 0;
        uint32_t r = 777;
        for (int i = 0; i < 5000; i++) {
            Inv t;
            itm_inv_init(&t);
            int m = itm_award_lot(&t, &db, 25002200, &r, got, 8);
            if (m == 1 && got[0].added == 1) hits++;
            else if (m) other++;
        }
        printf("lot 25002200: %d / 5000 drops (expected ~2%%)\n", hits);
        CHECK(hits > 50 && hits < 160 && other == 0, "drop rate %d", hits);
    }
    /* every class and gift is in the table, every class has its R1 */
    static const float MENU_WEIGHT[10] = {21.4f, 31.0f, 11.6f, 12.1f, 14.9f, 14.3f, 9.0f, 10.0f, 12.0f, 4.5f};  /* menu.json */
    for (int row = 3000; row <= 3009; row++) {
        Inv t;
        itm_new_game(&t, &db, row, 2401, 0);
        CHECK(itm_equipped(&t, ITM_SLOT_R1) >= 0, "class %d R1", row);
        const ItmClass *c = itm_find_class(&db, row);
        itm_calc(&t, &db, c->attr, &st);
        CHECK(NEAR(st.load, MENU_WEIGHT[row - 3000], 0.05f), "class %d load %.2f vs menu %.1f", row, st.load,
              MENU_WEIGHT[row - 3000]);
        printf("class %d: %2d entries, load %4.1f/%4.1f roll %d, hp %4.0f, AR R %5.1f L %5.1f, def %3.0f, poise %2.0f, %s\n",
               row, t.n, st.load, st.load_max, st.roll, st.hp_max, st.ar_r[0], st.ar_l[0], st.def[0], st.poise,
               itm_name(&db, itm_right_weapon(&t)));
    }
    for (int row = 2400; row <= 2408; row++) CHECK(itm_find_class(&db, row) != 0, "gift %d", row);

    printf("TEST:{\"suite\":\"items\",\"checks\":%d,\"fail\":%d}\n", checks, fails);
    free(file);
    return fails;
}
