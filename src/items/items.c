/* Ash items: database, inventory, equipment, stats, use, loot, save (items.h). Plain C99. */
#include "items.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

typedef char itm_chk_hdr[sizeof(ItmHdr) == 80 ? 1 : -1];
typedef char itm_chk_rec[sizeof(ItmRec) == 64 ? 1 : -1];
typedef char itm_chk_lot[sizeof(ItmLot) == 12 ? 1 : -1];
typedef char itm_chk_li[sizeof(ItmLotItem) == 8 ? 1 : -1];
typedef char itm_chk_cls[sizeof(ItmClass) == 16 ? 1 : -1];
typedef char itm_chk_st[sizeof(ItmStart) == 8 ? 1 : -1];
typedef char itm_chk_gr[sizeof(ItmGraph) == 64 ? 1 : -1];

/* ------------------------------------------------------------------ database */

static int sect_ok(int size, uint32_t off, uint32_t n, uint32_t es)
{
    return off >= sizeof(ItmHdr) && (off & 3) == 0 && off <= (uint32_t)size && n <= ((uint32_t)size - off) / es;
}

int itm_db_load(ItmDb *db, const void *file, int size)
{
    memset(db, 0, sizeof(*db));
    if (!file || size < (int)sizeof(ItmHdr)) return -1;
    const ItmHdr *h = (const ItmHdr *)file;
    if (memcmp(h->magic, "ASIT", 4) != 0 || h->version != 1) return -2;
    if (!sect_ok(size, h->off_item, h->n_item, sizeof(ItmRec)) ||
        !sect_ok(size, h->off_lot, h->n_lot, sizeof(ItmLot)) ||
        !sect_ok(size, h->off_lotitem, h->n_lotitem, sizeof(ItmLotItem)) ||
        !sect_ok(size, h->off_class, h->n_class, sizeof(ItmClass)) ||
        !sect_ok(size, h->off_start, h->n_start, sizeof(ItmStart)) ||
        !sect_ok(size, h->off_graph, h->n_graph, sizeof(ItmGraph)) ||
        !sect_ok(size, h->off_str, h->str_size, 1) || h->str_size == 0)
        return -3;
    const uint8_t *f = (const uint8_t *)file;
    if (f[h->off_str + h->str_size - 1] != 0) return -4;
    db->file = f;
    db->size = size;
    db->h = h;
    db->item = (const ItmRec *)(f + h->off_item);
    db->lot = (const ItmLot *)(f + h->off_lot);
    db->lotitem = (const ItmLotItem *)(f + h->off_lotitem);
    db->cls = (const ItmClass *)(f + h->off_class);
    db->start = (const ItmStart *)(f + h->off_start);
    db->graph = (const ItmGraph *)(f + h->off_graph);
    db->str = (const char *)(f + h->off_str);
    for (uint32_t i = 0; i < h->n_lot; i++)
        if ((uint32_t)db->lot[i].first + db->lot[i].n > h->n_lotitem) return -5;
    for (uint32_t i = 0; i < h->n_class; i++)
        if ((uint32_t)db->cls[i].first + db->cls[i].n > h->n_start) return -5;
    for (uint32_t i = 0; i < h->n_item; i++)
        if (db->item[i].name >= h->str_size || db->item[i].desc >= h->str_size) return -6;
    return 0;
}

const ItmRec *itm_find(const ItmDb *db, uint32_t key)
{
    if (!db->h) return 0;
    int lo = 0, hi = (int)db->h->n_item - 1;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        uint32_t k = db->item[mid].key;
        if (k == key) return &db->item[mid];
        if (k < key) lo = mid + 1; else hi = mid - 1;
    }
    return 0;
}

const char *itm_str(const ItmDb *db, uint32_t off)
{
    if (!db->h || off >= db->h->str_size) return "";
    return db->str + off;
}

const char *itm_name(const ItmDb *db, uint32_t key)
{
    const ItmRec *r = itm_find(db, key);
    return r ? itm_str(db, r->name) : "";
}

const ItmLot *itm_find_lot(const ItmDb *db, uint32_t lot_id)
{
    if (!db->h) return 0;
    int lo = 0, hi = (int)db->h->n_lot - 1;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        uint32_t k = db->lot[mid].lot_id;
        if (k == lot_id) return &db->lot[mid];
        if (k < lot_id) lo = mid + 1; else hi = mid - 1;
    }
    return 0;
}

const ItmClass *itm_find_class(const ItmDb *db, int row)
{
    if (!db->h) return 0;
    for (uint32_t i = 0; i < db->h->n_class; i++)
        if (db->cls[i].row == row) return &db->cls[i];
    return 0;
}

/* ------------------------------------------------------------------ inventory */

void itm_inv_init(Inv *inv)
{
    memset(inv, 0, sizeof(*inv));
    for (int i = 0; i < ITM_SLOT_COUNT; i++) inv->eq[i] = -1;
}

int itm_count(const Inv *inv, uint32_t key)
{
    int n = 0;
    for (int i = 0; i < inv->n; i++)
        if (inv->e[i].key == key) n += inv->e[i].count;
    return n;
}

int itm_has(const Inv *inv, uint32_t key) { return itm_count(inv, key) > 0; }

int itm_find_entry(const Inv *inv, uint32_t key)
{
    for (int i = 0; i < inv->n; i++)
        if (inv->e[i].key == key) return i;
    return -1;
}

static int is_estus(const ItmRec *r) { return r && r->cat == ITM_CAT_GOODS && r->kind == ITM_GK_ESTUS; }

static int new_entry(Inv *inv, uint32_t key, int count)
{
    if (inv->n >= ITM_INV_MAX) return -1;
    ItmEntry *e = &inv->e[inv->n];
    e->key = key;
    e->count = (uint16_t)count;
    e->order = inv->next_order++;
    return inv->n++;
}

int itm_add(Inv *inv, const ItmDb *db, uint32_t key, int count)
{
    const ItmRec *r = itm_find(db, key);
    if (!r || count <= 0) return 0;
    if (is_estus(r)) {                       /* one flask: empty/full variants are the same item for us */
        for (int i = 0; i < inv->n; i++)
            if (is_estus(itm_find(db, inv->e[i].key))) return 0;
        return new_entry(inv, key, 1) >= 0 ? 1 : 0;
    }
    int added = 0;
    int stack = r->cat == ITM_CAT_GOODS || (r->cat == ITM_CAT_WEAPON && (r->kind == ITM_WK_ARROW || r->kind == ITM_WK_BOLT));
    if (stack) {
        int max = r->max_hold ? r->max_hold : 1;
        if (r->flags & ITM_F_UNIQUE) max = 1;
        int i = itm_find_entry(inv, key);
        if (i < 0) {
            int c = count < max ? count : max;
            if (new_entry(inv, key, c) < 0) return 0;
            return c;
        }
        int room = max - inv->e[i].count;
        added = count < room ? count : room;
        if (added > 0) inv->e[i].count = (uint16_t)(inv->e[i].count + added);
        return added > 0 ? added : 0;
    }
    for (int k = 0; k < count; k++) {
        if (new_entry(inv, key, 1) < 0) break;
        added++;
    }
    return added;
}

static void drop_entry(Inv *inv, int idx)
{
    for (int s = 0; s < ITM_SLOT_COUNT; s++) {
        if (inv->eq[s] == idx) inv->eq[s] = -1;
        else if (inv->eq[s] > idx) inv->eq[s]--;
    }
    memmove(&inv->e[idx], &inv->e[idx + 1], (size_t)(inv->n - idx - 1) * sizeof(ItmEntry));
    inv->n--;
}

int itm_remove_at(Inv *inv, int idx, int count)
{
    if (idx < 0 || idx >= inv->n || count <= 0) return 0;
    int c = inv->e[idx].count;
    int r = count < c ? count : c;
    if (r == c) drop_entry(inv, idx);
    else inv->e[idx].count = (uint16_t)(c - r);
    return r;
}

int itm_remove(Inv *inv, uint32_t key, int count)
{
    int done = 0;
    for (int i = inv->n - 1; i >= 0 && done < count; i--)
        if (inv->e[i].key == key) done += itm_remove_at(inv, i, count - done);
    return done;
}

/* ------------------------------------------------------------------ equipment */

int itm_slot_accepts(const ItmDb *db, int slot, const ItmRec *r)
{
    (void)db;
    if (!r) return 0;
    int ammo = r->cat == ITM_CAT_WEAPON && (r->kind == ITM_WK_ARROW || r->kind == ITM_WK_BOLT);
    switch (slot) {
    case ITM_SLOT_R1: case ITM_SLOT_R2:
        return r->cat == ITM_CAT_WEAPON && !ammo && (r->flags & ITM_F_RIGHT);
    case ITM_SLOT_L1: case ITM_SLOT_L2:
        return r->cat == ITM_CAT_WEAPON && !ammo && (r->flags & ITM_F_LEFT);
    case ITM_SLOT_ARROW1: case ITM_SLOT_ARROW2:
        return r->cat == ITM_CAT_WEAPON && r->kind == ITM_WK_ARROW;
    case ITM_SLOT_BOLT1: case ITM_SLOT_BOLT2:
        return r->cat == ITM_CAT_WEAPON && r->kind == ITM_WK_BOLT;
    case ITM_SLOT_HEAD: case ITM_SLOT_BODY: case ITM_SLOT_ARMS: case ITM_SLOT_LEGS:
        return r->cat == ITM_CAT_ARMOR && r->kind == slot - ITM_SLOT_HEAD;
    case ITM_SLOT_RING1: case ITM_SLOT_RING2:
        return r->cat == ITM_CAT_RING;
    case ITM_SLOT_Q1: case ITM_SLOT_Q2: case ITM_SLOT_Q3: case ITM_SLOT_Q4: case ITM_SLOT_Q5:
        return r->cat == ITM_CAT_GOODS && (r->flags & ITM_F_QUICK);
    }
    return 0;
}

int itm_slot_of(const Inv *inv, int idx)
{
    for (int s = 0; s < ITM_SLOT_COUNT; s++)
        if (inv->eq[s] == idx) return s;
    return -1;
}

int itm_can_equip(const Inv *inv, const ItmDb *db, int slot, int idx)
{
    if (slot < 0 || slot >= ITM_SLOT_COUNT) return ITM_ERR_SLOT;
    if (idx < 0 || idx >= inv->n) return ITM_ERR_INDEX;
    const ItmRec *r = itm_find(db, inv->e[idx].key);
    if (!r) return ITM_ERR_UNKNOWN;
    if (!itm_slot_accepts(db, slot, r)) return ITM_ERR_SLOT;
    if (slot == ITM_SLOT_RING1 || slot == ITM_SLOT_RING2) {   /* the same ring cannot be worn twice (DS1) */
        int other = slot == ITM_SLOT_RING1 ? ITM_SLOT_RING2 : ITM_SLOT_RING1;
        int o = inv->eq[other];
        if (o >= 0 && o != idx && inv->e[o].key == inv->e[idx].key) return ITM_ERR_SLOT;
    }
    return ITM_OK;
}

int itm_equip(Inv *inv, const ItmDb *db, int slot, int idx)
{
    int rc = itm_can_equip(inv, db, slot, idx);
    if (rc != ITM_OK) return rc;
    int from = itm_slot_of(inv, idx);
    int old = inv->eq[slot];
    if (from >= 0 && from != slot) {
        /* moved from another slot: that slot takes our old item when it fits there (DS1 swap), else stays empty */
        inv->eq[from] = -1;
        if (old >= 0 && itm_can_equip(inv, db, from, old) == ITM_OK) inv->eq[from] = (int8_t)old;
    }
    inv->eq[slot] = (int8_t)idx;
    return ITM_OK;
}

void itm_unequip(Inv *inv, int slot)
{
    if (slot >= 0 && slot < ITM_SLOT_COUNT) inv->eq[slot] = -1;
}

int itm_equipped(const Inv *inv, int slot)
{
    return slot >= 0 && slot < ITM_SLOT_COUNT ? inv->eq[slot] : -1;
}

uint32_t itm_equipped_key(const Inv *inv, int slot)
{
    int i = itm_equipped(inv, slot);
    return i >= 0 && i < inv->n ? inv->e[i].key : 0;
}

int itm_candidates(const Inv *inv, const ItmDb *db, int slot, int *out, int max)
{
    int n = 0;
    for (int i = 0; i < inv->n && n < max; i++)
        if (itm_slot_accepts(db, slot, itm_find(db, inv->e[i].key))) out[n++] = i;
    return n;
}

uint32_t itm_right_weapon(const Inv *inv) { return itm_equipped_key(inv, inv->r_active ? ITM_SLOT_R2 : ITM_SLOT_R1); }
uint32_t itm_left_weapon(const Inv *inv) { return itm_equipped_key(inv, inv->l_active ? ITM_SLOT_L2 : ITM_SLOT_L1); }

void itm_switch_right(Inv *inv)
{
    int to = !inv->r_active;
    if (inv->eq[to ? ITM_SLOT_R2 : ITM_SLOT_R1] >= 0 || inv->eq[inv->r_active ? ITM_SLOT_R2 : ITM_SLOT_R1] < 0)
        inv->r_active = (int8_t)to;
}

void itm_switch_left(Inv *inv)
{
    int to = !inv->l_active;
    if (inv->eq[to ? ITM_SLOT_L2 : ITM_SLOT_L1] >= 0 || inv->eq[inv->l_active ? ITM_SLOT_L2 : ITM_SLOT_L1] < 0)
        inv->l_active = (int8_t)to;
}

int itm_quick_next(Inv *inv)
{
    for (int k = 1; k <= 5; k++) {
        int q = (inv->quick + k) % 5;
        if (inv->eq[ITM_SLOT_Q1 + q] >= 0) { inv->quick = (int8_t)q; break; }
    }
    return ITM_SLOT_Q1 + inv->quick;
}

int itm_quick_entry(const Inv *inv) { return inv->eq[ITM_SLOT_Q1 + inv->quick]; }

/* ------------------------------------------------------------------ new game */

int itm_new_game(Inv *inv, const ItmDb *db, int class_row, int gift_row, int asylum)
{
    itm_inv_init(inv);
    int added = 0;
    const ItmClass *c = itm_find_class(db, class_row);
    if (c) {
        inv->humanity = c->humanity;
        for (int i = 0; i < c->n; i++) {
            const ItmStart *s = &db->start[c->first + i];
            if (s->asylum == 0 && asylum) continue;
            if (s->asylum == 2 && !asylum) continue;
            if (itm_add(inv, db, s->key, s->count) <= 0) continue;
            added++;
            if (s->slot < ITM_SLOT_COUNT) {
                int idx = -1;
                for (int k = inv->n - 1; k >= 0; k--)
                    if (inv->e[k].key == s->key) { idx = k; break; }
                if (idx >= 0) itm_equip(inv, db, s->slot, idx);
            }
        }
    }
    const ItmClass *g = gift_row ? itm_find_class(db, gift_row) : 0;
    if (g)
        for (int i = 0; i < g->n; i++) {
            const ItmStart *s = &db->start[g->first + i];
            if (itm_add(inv, db, s->key, s->count) > 0) added++;
        }
    /* first non-empty quick slot is current */
    inv->quick = 4;
    itm_quick_next(inv);
    return added;
}

/* ------------------------------------------------------------------ stats */

float itm_correct(const ItmGraph *g, float stat)
{
    if (!g) return 0;
    if (stat <= g->x[0]) return g->y[0];
    for (int i = 0; i < 4; i++) {
        if (stat <= g->x[i + 1]) {
            float d = g->x[i + 1] - g->x[i];
            float r = (stat - g->x[i]) / (d > 1e-6f ? d : 1e-6f);
            float e = g->e[i];
            /* r^e for e > 0, 1-(1-r)^-e for e < 0 (same shape as tools/cook_combat.py correct()) */
            float p = e > 0 ? (r > 0 ? powf(r, e) : 0) : e < 0 ? 1 - (r < 1 ? powf(1 - r, -e) : 0) : 1;
            return g->y[i] + (g->y[i + 1] - g->y[i]) * p;
        }
    }
    return g->y[4];
}

static const ItmGraph *graph_by_id(const ItmDb *db, int id)
{
    if (!db->h || !db->h->n_graph) return 0;
    for (uint32_t i = 0; i < db->h->n_graph; i++)
        if (db->graph[i].id == id) return &db->graph[i];
    return &db->graph[0];
}

/* DS1 HP by VIT [версия: опорные точки по памяти; 14 -> 572 = tools/cook_combat.py PLAYER_HP_BY_VIT] */
float itm_hp_for_vit(int vit)
{
    static const struct { int v; float hp; } T[] = {{1, 400}, {8, 476}, {20, 668}, {30, 1100}, {50, 1500}, {99, 1900}};
    if (vit <= T[0].v) return T[0].hp;
    for (int i = 0; i + 1 < (int)(sizeof(T) / sizeof(T[0])); i++)
        if (vit <= T[i + 1].v) return T[i].hp + (T[i + 1].hp - T[i].hp) * (vit - T[i].v) / (float)(T[i + 1].v - T[i].v);
    return 1900;
}

/* DS1 stamina by END [версия: 8..12 = tools/cook_combat.py PLAYER_STAMINA_BY_END, 40 -> 160 cap по памяти] */
float itm_sp_for_end(int end)
{
    static const struct { int e; float sp; } T[] = {{1, 81}, {8, 89}, {9, 90}, {10, 91}, {11, 93}, {12, 95}, {40, 160}};
    if (end <= T[0].e) return T[0].sp;
    for (int i = 0; i + 1 < (int)(sizeof(T) / sizeof(T[0])); i++)
        if (end <= T[i + 1].e) return T[i].sp + (T[i + 1].sp - T[i].sp) * (end - T[i].e) / (float)(T[i + 1].e - T[i].e);
    return 160;
}

enum { A_VIT, A_ATT, A_END, A_STR, A_DEX, A_RES, A_INT, A_FTH };

float itm_weapon_ar(const ItmDb *db, const ItmRec *w, const uint8_t attr[8], int two_hand, float ar[4], int *req_ok)
{
    ar[0] = ar[1] = ar[2] = ar[3] = 0;
    if (req_ok) *req_ok = 1;
    if (!w || w->cat != ITM_CAT_WEAPON) return 0;
    float str = attr[A_STR] * (two_hand ? 1.5f : 1.0f);     /* two-handing: STR x1.5 (DS1) */
    float st[4] = {str, attr[A_DEX], attr[A_INT], attr[A_FTH]};
    int ok = 1;
    for (int i = 0; i < 4; i++)
        if (w->v[ITM_WV_REQ + i] > 0 && st[i] < w->v[ITM_WV_REQ + i]) ok = 0;
    if (req_ok) *req_ok = ok;
    const ItmGraph *g = graph_by_id(db, w->v[ITM_WV_GRAPH]);
    float c[4];
    for (int i = 0; i < 4; i++) c[i] = itm_correct(g, st[i]) / 100.0f;
    float bp = w->v[ITM_WV_ATK + 0], bm = w->v[ITM_WV_ATK + 1];
    if (ok) {
        /* AR = base + base * scaling% * correct% (tools/cook_combat.py; magic scales with INT and FTH) [версия] */
        ar[0] = bp + bp * (w->v[ITM_WV_SCALE + 0] / 100.0f * c[0] + w->v[ITM_WV_SCALE + 1] / 100.0f * c[1]);
        ar[1] = bm + bm * (w->v[ITM_WV_SCALE + 2] / 100.0f * c[2] + w->v[ITM_WV_SCALE + 3] / 100.0f * c[3]);
        ar[2] = w->v[ITM_WV_ATK + 2];
        ar[3] = w->v[ITM_WV_ATK + 3];
    } else {
        /* requirement not met: no scaling and a penalty [версия: DS1 «штраф», точный множитель не найден] */
        for (int i = 0; i < 4; i++) ar[i] = w->v[ITM_WV_ATK + i] * 0.6f;
    }
    return ar[0] + ar[1] + ar[2] + ar[3];
}

void itm_calc(const Inv *inv, const ItmDb *db, const uint8_t attr[8], ItmStats *o)
{
    memset(o, 0, sizeof(*o));
    float load_mult = 1, hp_mult = 1, sp_mult = 1;
    for (int s = ITM_SLOT_RING1; s <= ITM_SLOT_RING2; s++) {
        const ItmRec *r = itm_find(db, itm_equipped_key(inv, s));
        if (!r) continue;
        load_mult *= r->v[ITM_RV_LOAD_PCT] / 100.0f;
        hp_mult *= r->v[ITM_RV_HP_PCT] / 100.0f;
        sp_mult *= r->v[ITM_RV_SP_PCT] / 100.0f;
        o->poise += r->v[ITM_RV_POISE];
        for (int i = 0; i < 4; i++) o->def[i] += r->v[ITM_RV_DEF + i];
        o->load += r->weight;
    }
    o->hp_max = itm_hp_for_vit(attr[A_VIT]) * hp_mult;
    o->sp_max = itm_sp_for_end(attr[A_END]) * sp_mult;
    o->load_max = (40.0f + attr[A_END]) * load_mult;   /* DS1: 40 + END [версия: по памяти] */
    for (int s = ITM_SLOT_R1; s <= ITM_SLOT_BOLT2; s++) {
        const ItmRec *r = itm_find(db, itm_equipped_key(inv, s));
        if (r) o->load += r->weight;
    }
    for (int s = ITM_SLOT_HEAD; s <= ITM_SLOT_LEGS; s++) {
        const ItmRec *r = itm_find(db, itm_equipped_key(inv, s));
        if (!r) continue;
        o->load += r->weight;
        for (int i = 0; i < 4; i++) o->def[i] += r->v[ITM_AV_DEF + i];
        o->def_strike += r->v[ITM_AV_STRIKE];
        o->def_slash += r->v[ITM_AV_SLASH];
        o->def_thrust += r->v[ITM_AV_THRUST];
        o->poise += r->v[ITM_AV_POISE];
        o->res_bleed += r->v[ITM_AV_BLEED];
        o->res_poison += r->v[ITM_AV_POISON];
        o->res_curse += r->v[ITM_AV_CURSE];
    }
    o->load_ratio = o->load_max > 0 ? o->load / o->load_max : 0;
    o->roll = o->load_ratio <= 0.25f ? ITM_ROLL_FAST : o->load_ratio <= 0.5f ? ITM_ROLL_MID :
              o->load_ratio <= 1.0f ? ITM_ROLL_FAT : ITM_ROLL_OVER;
    itm_weapon_ar(db, itm_find(db, itm_right_weapon(inv)), attr, inv->two_hand, o->ar_r, &o->req_ok_r);
    itm_weapon_ar(db, itm_find(db, itm_left_weapon(inv)), attr, 0, o->ar_l, &o->req_ok_l);
    if (!itm_right_weapon(inv)) o->req_ok_r = 1;
    if (!itm_left_weapon(inv)) o->req_ok_l = 1;
    const ItmRec *l = itm_find(db, itm_left_weapon(inv));
    if (l) {
        for (int i = 0; i < 4; i++) o->guard_l[i] = l->v[ITM_WV_GUARD + i];
        o->stability_l = l->v[ITM_WV_STABILITY];
    }
}

/* ------------------------------------------------------------------ use */

int itm_use(Inv *inv, const ItmDb *db, int idx, ItmUse *out)
{
    memset(out, 0, sizeof(*out));
    if (idx < 0 || idx >= inv->n) return ITM_ERR_INDEX;
    const ItmRec *r = itm_find(db, inv->e[idx].key);
    if (!r) return ITM_ERR_UNKNOWN;
    if (r->cat != ITM_CAT_GOODS || r->use == ITM_USE_NONE || r->use == ITM_USE_KEY) return ITM_ERR_USE;
    out->kind = r->use;
    out->value = r->use_val;
    out->key = r->key;
    out->anim = r->v[ITM_GV_ANIM];
    if (r->use == ITM_USE_ESTUS) return ITM_OK;               /* charges: src/progress prg_try_estus */
    if (r->use == ITM_USE_HUMANITY) inv->humanity += r->use_val;
    if (r->flags & ITM_F_CONSUMED) out->consumed = itm_remove_at(inv, idx, 1);
    return ITM_OK;
}

int itm_use_quick(Inv *inv, const ItmDb *db, ItmUse *out)
{
    int idx = itm_quick_entry(inv);
    if (idx < 0) { memset(out, 0, sizeof(*out)); return ITM_ERR_INDEX; }
    int rc = itm_use(inv, db, idx, out);
    if (rc == ITM_OK && inv->eq[ITM_SLOT_Q1 + inv->quick] < 0) itm_quick_next(inv);   /* stack used up */
    return rc;
}

/* ------------------------------------------------------------------ loot */

static uint32_t xorshift(uint32_t *s)
{
    uint32_t x = *s ? *s : 0x9e3779b9u;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return *s = x;
}

int itm_award_lot(Inv *inv, const ItmDb *db, uint32_t lot_id, uint32_t *rng, ItmGot *got, int max_got)
{
    const ItmLot *L = itm_find_lot(db, lot_id);
    if (!L || max_got <= 0) return 0;
    const ItmLotItem *li = &db->lotitem[L->first];
    int random = 0, total = 0;
    for (int i = 0; i < L->n; i++) {
        total += li[i].chance;
        if (ITM_KEY_ID(li[i].key) == 0 && li[i].chance > 0) random = 1;
    }
    int n = 0;
    if (random && total > 0) {
        uint32_t seed = 1;
        int roll = (int)(xorshift(rng ? rng : &seed) % (uint32_t)total);
        for (int i = 0; i < L->n; i++) {
            if (roll < li[i].chance) {
                if (ITM_KEY_ID(li[i].key) == 0) return 0;      /* "nothing" */
                got[0].key = li[i].key;
                got[0].count = li[i].count;
                got[0].added = itm_add(inv, db, li[i].key, li[i].count);
                return 1;
            }
            roll -= li[i].chance;
        }
        return 0;
    }
    for (int i = 0; i < L->n && n < max_got; i++) {
        if (ITM_KEY_ID(li[i].key) == 0) continue;
        got[n].key = li[i].key;
        got[n].count = li[i].count;
        got[n].added = itm_add(inv, db, li[i].key, li[i].count);
        n++;
    }
    return n;
}

/* snprintf that never leaves half a UTF-8 character when it truncates */
static int put_utf8(char *buf, int cap, int n)
{
    if (cap > 0 && n >= cap) {
        int j = cap - 2, end = cap - 1;
        while (j > 0 && ((unsigned char)buf[j] & 0xc0) == 0x80) j--;
        unsigned char c = j >= 0 ? (unsigned char)buf[j] : 0;
        int len = c < 0x80 ? 1 : (c & 0xe0) == 0xc0 ? 2 : (c & 0xf0) == 0xe0 ? 3 : 4;
        if (j >= 0 && j + len > end) end = j;
        buf[end] = 0;
        n = end;
    }
    return n;
}

static int put_prefixed(const ItmDb *db, uint32_t tmpl, const char *body, char *buf, int cap)
{
    const char *t = itm_str(db, tmpl);
    const char *mark = strchr(t, '\x01');
    int pre = mark ? (int)(mark - t) : (int)strlen(t);
    return put_utf8(buf, cap, snprintf(buf, (size_t)cap, "%.*s%s%s", pre, t, body, mark ? mark + 1 : ""));
}

int itm_got_text(const ItmDb *db, const ItmGot *got, int n, char *buf, int cap)
{
    char body[256], piece[160];
    int o = 0, any = 0;
    body[0] = 0;
    /* room for the names = cap minus the template ("Получено: \x01") minus the NUL, whole items only */
    int room = (int)sizeof body;
    if (db->h) {
        int tl = (int)strlen(itm_str(db, db->h->s_got)) - 1;
        if (cap - tl - 1 < room) room = cap - tl - 1;
    }
    for (int i = 0; i < n; i++) {
        if (got[i].added <= 0) continue;
        const char *nm = itm_name(db, got[i].key);
        int k = got[i].added > 1 ? snprintf(piece, sizeof piece, "%s%s x%d", any ? ", " : "", nm, got[i].added)
                                 : snprintf(piece, sizeof piece, "%s%s", any ? ", " : "", nm);
        /* whole items only: never cut a UTF-8 name in the middle */
        if (k < 0 || k >= (int)sizeof piece || o + k + 4 > room) {
            if (any && o + 4 <= room) { memcpy(body + o, "\xe2\x80\xa6", 4); o += 3; }   /* "…" */
            break;
        }
        memcpy(body + o, piece, (size_t)k + 1);
        o += k;
        any = 1;
    }
    if (!any) {
        for (int i = 0; i < n; i++)              /* added, but even the first name is too long: cut it */
            if (got[i].added > 0) return put_prefixed(db, db->h->s_got, itm_name(db, got[i].key), buf, cap);
        return put_utf8(buf, cap, snprintf(buf, (size_t)cap, "%s", itm_str(db, db->h ? db->h->s_full : 0)));
    }
    return put_prefixed(db, db->h->s_got, body, buf, cap);
}

int itm_used_text(const ItmDb *db, uint32_t key, char *buf, int cap)
{
    if (!db->h) return snprintf(buf, (size_t)cap, "%s", "");
    return put_prefixed(db, db->h->s_used, itm_name(db, key), buf, cap);
}

/* ------------------------------------------------------------------ save */

#define SAVE_MAGIC 0x534d5449u   /* "ITMS" */

static void w32(uint8_t **p, uint32_t v) { (*p)[0] = (uint8_t)v; (*p)[1] = (uint8_t)(v >> 8); (*p)[2] = (uint8_t)(v >> 16); (*p)[3] = (uint8_t)(v >> 24); *p += 4; }
static void w16(uint8_t **p, uint32_t v) { (*p)[0] = (uint8_t)v; (*p)[1] = (uint8_t)(v >> 8); *p += 2; }
static uint32_t r32(const uint8_t **p) { uint32_t v = (*p)[0] | ((*p)[1] << 8) | ((uint32_t)(*p)[2] << 16) | ((uint32_t)(*p)[3] << 24); *p += 4; return v; }
static uint32_t r16(const uint8_t **p) { uint32_t v = (*p)[0] | ((*p)[1] << 8); *p += 2; return v; }

int itm_save_size(void) { return 4 + 4 + 4 + ITM_INV_MAX * 8 + ITM_SLOT_COUNT + 4 + 4 + 4; }

int itm_save_write(const Inv *inv, void *buf, int cap)
{
    if (cap < itm_save_size()) return -1;
    uint8_t *p = (uint8_t *)buf;
    memset(p, 0, (size_t)itm_save_size());
    w32(&p, SAVE_MAGIC);
    w32(&p, ITM_SAVE_VERSION);
    w32(&p, (uint32_t)inv->n);
    for (int i = 0; i < ITM_INV_MAX; i++) {
        w32(&p, i < inv->n ? inv->e[i].key : 0);
        w16(&p, i < inv->n ? inv->e[i].count : 0);
        w16(&p, i < inv->n ? inv->e[i].order : 0);
    }
    for (int s = 0; s < ITM_SLOT_COUNT; s++) *p++ = (uint8_t)inv->eq[s];
    *p++ = (uint8_t)inv->r_active; *p++ = (uint8_t)inv->l_active; *p++ = (uint8_t)inv->two_hand; *p++ = (uint8_t)inv->quick;
    w32(&p, (uint32_t)inv->humanity);
    w32(&p, inv->next_order);
    return (int)(p - (uint8_t *)buf);
}

int itm_save_read(Inv *inv, const ItmDb *db, const void *buf, int len)
{
    if (len < itm_save_size()) return -1;
    const uint8_t *p = (const uint8_t *)buf;
    if (r32(&p) != SAVE_MAGIC) return -2;
    if (r32(&p) != ITM_SAVE_VERSION) return -3;
    Inv t;
    itm_inv_init(&t);
    uint32_t n = r32(&p);
    if (n > ITM_INV_MAX) return -4;
    t.n = (int)n;
    for (int i = 0; i < ITM_INV_MAX; i++) {
        t.e[i].key = r32(&p);
        t.e[i].count = (uint16_t)r16(&p);
        t.e[i].order = (uint16_t)r16(&p);
        if (i < t.n && (!t.e[i].count || (db && !itm_find(db, t.e[i].key)))) return -5;
    }
    for (int s = 0; s < ITM_SLOT_COUNT; s++) {
        t.eq[s] = (int8_t)*p++;
        if (t.eq[s] >= t.n || t.eq[s] < -1) return -6;
    }
    t.r_active = (int8_t)(*p++ & 1); t.l_active = (int8_t)(*p++ & 1); t.two_hand = (int8_t)(*p++ & 1);
    t.quick = (int8_t)(*p++ % 5);
    t.humanity = (int32_t)r32(&p);
    t.next_order = (uint16_t)r32(&p);
    t.estus = inv->estus;
    *inv = t;
    return 0;
}
