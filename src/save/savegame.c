/* Ash save game: pack / unpack / compare (savegame.h). Plain C99. */
#include <math.h>
#include <string.h>

#include "savegame.h"
#include "save.h"

#define TAG_META SAVE_TAG('M', 'E', 'T', 'A')
#define TAG_CHR SAVE_TAG('C', 'H', 'R', '1')
#define TAG_STN SAVE_TAG('S', 'T', 'N', '1')
#define TAG_INV SAVE_TAG('I', 'N', 'V', '1')
#define TAG_SET SAVE_TAG('S', 'E', 'T', '1')
#define TAG_ZEVT SAVE_TAG('Z', 'E', 'V', 'T')
#define TAG_ITMS SAVE_TAG('I', 'T', 'M', 'S')

void sg_init(SaveGame *s)
{
    memset(s, 0, sizeof(*s));
    for (int i = 0; i < SG_EQUIP_SLOTS; i++) s->equip[i] = -1;
    s->brightness = 5;   /* = MenuSettings defaults (menu.c) */
    s->cam_speed = 5;
    s->level = 1;
}

/* ---------------------------------------------------------------- little-endian writer / reader */

typedef struct { uint8_t *p; int n, cap, bad; } W;
static void w8(W *w, uint32_t v) { if (w->n + 1 > w->cap) { w->bad = 1; return; } w->p[w->n++] = (uint8_t)v; }
static void w16(W *w, uint32_t v) { w8(w, v); w8(w, v >> 8); }
static void w32(W *w, uint32_t v) { w16(w, v); w16(w, v >> 16); }
static void w64(W *w, uint64_t v) { w32(w, (uint32_t)v); w32(w, (uint32_t)(v >> 32)); }
static void wf(W *w, float f) { uint32_t u; memcpy(&u, &f, 4); w32(w, u); }
static void wb(W *w, const void *d, int n)
{
    if (w->n + n > w->cap) { w->bad = 1; return; }
    memcpy(w->p + w->n, d, n);
    w->n += n;
}
static void wal(W *w) { while (w->n & 3) w8(w, 0); }

typedef struct { const uint8_t *p; int n, len, bad; } R;
static uint32_t r8(R *r) { if (r->n + 1 > r->len) { r->bad = 1; return 0; } return r->p[r->n++]; }
static uint32_t r16(R *r) { uint32_t a = r8(r); return a | (r8(r) << 8); }
static uint32_t r32(R *r) { uint32_t a = r16(r); return a | (r16(r) << 16); }
static uint64_t r64(R *r) { uint64_t a = r32(r); return a | ((uint64_t)r32(r) << 32); }
static float rf(R *r) { uint32_t u = r32(r); float f; memcpy(&f, &u, 4); return f; }
static void rb(R *r, void *d, int n)
{
    if (n < 0 || r->n + n > r->len) { r->bad = 1; memset(d, 0, n > 0 ? n : 0); return; }
    memcpy(d, r->p + r->n, n);
    r->n += n;
}
static void ral(R *r) { while (r->n & 3) r8(r); }

/* ---------------------------------------------------------------- pack */

static uint8_t scratch[SG_MAX_BYTES];   /* parts are built here, then save_pack copies them (not re-entrant) */

int sg_pack(const SaveGame *s, void *buf, int cap)
{
    if (s->n_item < 0 || s->n_item > SG_MAX_ITEMS || s->n_zone < 0 || s->n_zone > SG_MAX_ZONES) return -6;
    W w = {scratch, 0, (int)sizeof(scratch), 0};
    SavePart part[7];
    int np = 0, o;

    o = w.n;
    w32(&w, SG_FORMAT); w32(&w, s->seq); w32(&w, s->play_s);
    w8(&w, s->area); w8(&w, s->block); w16(&w, 0);
    w32(&w, s->bonfire); w32(&w, s->flags); w64(&w, s->time);
    part[np++] = (SavePart){TAG_META, scratch + o, w.n - o};

    o = w.n;
    char name[SG_NAME_BYTES];
    memset(name, 0, sizeof(name));
    for (int i = 0; i < SG_NAME_BYTES - 1 && s->name[i]; i++) name[i] = s->name[i];   /* 0-terminated in the file */
    wb(&w, name, SG_NAME_BYTES);
    w16(&w, s->class_row); w8(&w, s->gender); w8(&w, s->gift); w8(&w, s->hair); w8(&w, 0); w16(&w, 0);
    w16(&w, s->level); w16(&w, 0);
    wb(&w, s->attr, 8);
    w32(&w, (uint32_t)s->humanity); w32(&w, (uint32_t)s->souls);
    wf(&w, s->hp); wf(&w, s->hp_max);
    w32(&w, (uint32_t)s->estus); w32(&w, (uint32_t)s->estus_max); w32(&w, (uint32_t)s->deaths);
    w32(&w, s->prg_flags);
    part[np++] = (SavePart){TAG_CHR, scratch + o, w.n - o};

    o = w.n;
    w32(&w, (uint32_t)s->stain_on); w8(&w, s->stain_area); w8(&w, s->stain_block); w16(&w, 0);
    for (int i = 0; i < 3; i++) wf(&w, s->stain_pos[i]);
    w32(&w, (uint32_t)s->stain_souls);
    part[np++] = (SavePart){TAG_STN, scratch + o, w.n - o};

    o = w.n;
    w16(&w, s->n_item); w16(&w, SG_EQUIP_SLOTS);
    for (int i = 0; i < s->n_item; i++) {
        w8(&w, s->item[i].cat); w8(&w, s->item[i].level); w16(&w, s->item[i].count); w32(&w, s->item[i].id);
    }
    for (int i = 0; i < SG_EQUIP_SLOTS; i++) w16(&w, (uint16_t)s->equip[i]);
    wal(&w);
    part[np++] = (SavePart){TAG_INV, scratch + o, w.n - o};

    o = w.n;
    w8(&w, s->scheme); w8(&w, s->brightness); w8(&w, s->cam_speed); w8(&w, s->invert_x); w8(&w, s->invert_y);
    w8(&w, s->overlay); w16(&w, 0);
    part[np++] = (SavePart){TAG_SET, scratch + o, w.n - o};

    o = w.n;
    w16(&w, s->n_zone); w16(&w, 0);
    for (int i = 0; i < s->n_zone; i++) {
        const SgZoneEvents *z = &s->zone[i];
        if (z->size < 0 || z->size > SG_EVT_BYTES) return -6;
        w8(&w, z->area); w8(&w, z->block); w16(&w, 0); w32(&w, (uint32_t)z->size);
        wb(&w, z->data, z->size);
        wal(&w);
    }
    part[np++] = (SavePart){TAG_ZEVT, scratch + o, w.n - o};

    if (s->itm_size < 0 || s->itm_size > SG_ITM_BYTES) return -6;
    if (s->itm_size > 0) {
        o = w.n;
        wb(&w, s->itm_blob, s->itm_size);
        wal(&w);
        part[np++] = (SavePart){TAG_ITMS, scratch + o, s->itm_size};
    }

    if (w.bad) return -1;
    return save_pack(buf, cap, part, np);
}

/* ---------------------------------------------------------------- unpack */

static int finite3(const float *v) { return isfinite(v[0]) && isfinite(v[1]) && isfinite(v[2]); }
static int sg_unpack_into(SaveGame *o, const void *buf, int len);

int sg_unpack(SaveGame *out, const void *buf, int len)
{
    int r = sg_unpack_into(out, buf, len);
    if (r != 0) sg_init(out);
    return r;
}

static int sg_unpack_into(SaveGame *o, const void *buf, int len)
{
#define s (*o)
    SavePart part[SAVE_MAX_PARTS];
    sg_init(o);
    int np = save_unpack(buf, len, part, SAVE_MAX_PARTS);
    if (np < 0) return np;
    const SavePart *pm = save_find(part, np, TAG_META), *pc = save_find(part, np, TAG_CHR);
    if (!pm || !pc) return -4;

    R r = {(const uint8_t *)pm->data, 0, pm->size, 0};
    uint32_t fmt = r32(&r);
    if (fmt > SG_FORMAT) return -5;
    s.seq = r32(&r); s.play_s = r32(&r);
    s.area = r8(&r); s.block = r8(&r); r16(&r);
    s.bonfire = r32(&r); s.flags = r32(&r); s.time = r64(&r);
    if (r.bad) return -6;

    r = (R){(const uint8_t *)pc->data, 0, pc->size, 0};
    rb(&r, s.name, SG_NAME_BYTES);
    s.name[SG_NAME_BYTES - 1] = 0;
    s.class_row = r16(&r); s.gender = r8(&r); s.gift = r8(&r); s.hair = r8(&r); r8(&r); r16(&r);
    s.level = r16(&r); r16(&r);
    rb(&r, s.attr, 8);
    s.humanity = (int32_t)r32(&r); s.souls = (int32_t)r32(&r);
    s.hp = rf(&r); s.hp_max = rf(&r);
    s.estus = (int32_t)r32(&r); s.estus_max = (int32_t)r32(&r); s.deaths = (int32_t)r32(&r);
    s.prg_flags = r32(&r);
    if (r.bad || s.souls < 0 || s.estus < 0 || s.level < 1 || !isfinite(s.hp) || !isfinite(s.hp_max)) return -6;

    const SavePart *pp = save_find(part, np, TAG_STN);
    if (pp) {
        r = (R){(const uint8_t *)pp->data, 0, pp->size, 0};
        s.stain_on = (int32_t)r32(&r); s.stain_area = r8(&r); s.stain_block = r8(&r); r16(&r);
        for (int i = 0; i < 3; i++) s.stain_pos[i] = rf(&r);
        s.stain_souls = (int32_t)r32(&r);
        if (r.bad || s.stain_souls < 0 || !finite3(s.stain_pos)) return -6;
    }
    pp = save_find(part, np, TAG_INV);
    if (pp) {
        r = (R){(const uint8_t *)pp->data, 0, pp->size, 0};
        int n = (int)r16(&r), ne = (int)r16(&r);
        if (n > SG_MAX_ITEMS) return -6;
        s.n_item = n;
        for (int i = 0; i < n; i++) {
            s.item[i].cat = r8(&r); s.item[i].level = r8(&r); s.item[i].count = r16(&r); s.item[i].id = r32(&r);
        }
        for (int i = 0; i < ne; i++) {
            int16_t e = (int16_t)r16(&r);
            if (e < -1 || e >= n) return -6;
            if (i < SG_EQUIP_SLOTS) s.equip[i] = e;
        }
        if (r.bad) return -6;
    }
    pp = save_find(part, np, TAG_SET);
    if (pp) {
        r = (R){(const uint8_t *)pp->data, 0, pp->size, 0};
        s.scheme = (int8_t)r8(&r); s.brightness = (int8_t)r8(&r); s.cam_speed = (int8_t)r8(&r);
        s.invert_x = (int8_t)r8(&r); s.invert_y = (int8_t)r8(&r); s.overlay = (int8_t)r8(&r);
        if (r.bad) return -6;
    }
    pp = save_find(part, np, TAG_ZEVT);
    if (pp) {
        r = (R){(const uint8_t *)pp->data, 0, pp->size, 0};
        int n = (int)r16(&r);
        r16(&r);
        if (n > SG_MAX_ZONES) return -6;
        s.n_zone = n;
        for (int i = 0; i < n; i++) {
            SgZoneEvents *z = &s.zone[i];
            z->area = r8(&r); z->block = r8(&r); r16(&r);
            uint32_t sz = r32(&r);
            if (sz > SG_EVT_BYTES) return -6;
            z->size = (int)sz;
            rb(&r, z->data, z->size);
            ral(&r);
        }
        if (r.bad) return -6;
    }
    pp = save_find(part, np, TAG_ITMS);
    if (pp) {
        if (pp->size > SG_ITM_BYTES) return -6;
        s.itm_size = pp->size;
        memcpy(s.itm_blob, pp->data, pp->size);
    }
    return 0;
#undef s
}

/* ---------------------------------------------------------------- helpers */

SgZoneEvents *sg_zone_put(SaveGame *s, int area, int block)
{
    for (int i = 0; i < s->n_zone; i++)
        if (s->zone[i].area == area && s->zone[i].block == block) return &s->zone[i];
    if (s->n_zone >= SG_MAX_ZONES) return 0;
    SgZoneEvents *z = &s->zone[s->n_zone++];
    z->area = (uint8_t)area;
    z->block = (uint8_t)block;
    z->size = 0;
    return z;
}

const SgZoneEvents *sg_zone_get(const SaveGame *s, int area, int block)
{
    for (int i = 0; i < s->n_zone; i++)
        if (s->zone[i].area == area && s->zone[i].block == block) return &s->zone[i];
    return 0;
}

static int feq(float a, float b) { return memcmp(&a, &b, 4) == 0; }

int sg_compare(const SaveGame *a, const SaveGame *b, const char **what)
{
    const char *w = 0;
    int k = 0, first = 0;
#define CHK(cond, name) do { k++; if (!w && !(cond)) { w = name; first = k; } } while (0)
    CHK(a->seq == b->seq && a->play_s == b->play_s && a->time == b->time, "meta.seq/play_s/time");
    CHK(a->area == b->area && a->block == b->block && a->bonfire == b->bonfire && a->flags == b->flags,
        "meta.zone/bonfire/flags");
    CHK(strncmp(a->name, b->name, SG_NAME_BYTES) == 0, "chr.name");
    CHK(a->class_row == b->class_row && a->gender == b->gender && a->gift == b->gift && a->hair == b->hair,
        "chr.class/gender/gift/hair");
    CHK(a->level == b->level && memcmp(a->attr, b->attr, 8) == 0, "chr.level/attr");
    CHK(a->humanity == b->humanity && a->souls == b->souls && a->deaths == b->deaths, "chr.humanity/souls/deaths");
    CHK(feq(a->hp, b->hp) && feq(a->hp_max, b->hp_max) && a->estus == b->estus && a->estus_max == b->estus_max &&
        a->prg_flags == b->prg_flags, "chr.hp/estus/flags");
    CHK(a->stain_on == b->stain_on && a->stain_area == b->stain_area && a->stain_block == b->stain_block &&
        feq(a->stain_pos[0], b->stain_pos[0]) && feq(a->stain_pos[1], b->stain_pos[1]) &&
        feq(a->stain_pos[2], b->stain_pos[2]) && a->stain_souls == b->stain_souls, "stain");
    int inv = a->n_item == b->n_item;
    for (int i = 0; inv && i < a->n_item; i++)
        inv = a->item[i].cat == b->item[i].cat && a->item[i].level == b->item[i].level &&
              a->item[i].count == b->item[i].count && a->item[i].id == b->item[i].id;
    CHK(inv, "inv.items");
    CHK(memcmp(a->equip, b->equip, sizeof(a->equip)) == 0, "inv.equip");
    CHK(a->scheme == b->scheme && a->brightness == b->brightness && a->cam_speed == b->cam_speed &&
        a->invert_x == b->invert_x && a->invert_y == b->invert_y && a->overlay == b->overlay, "settings");
    int ze = a->n_zone == b->n_zone;
    for (int i = 0; ze && i < a->n_zone; i++)
        ze = a->zone[i].area == b->zone[i].area && a->zone[i].block == b->zone[i].block &&
             a->zone[i].size == b->zone[i].size && memcmp(a->zone[i].data, b->zone[i].data, a->zone[i].size) == 0;
    CHK(ze, "zone events");
    CHK(a->itm_size == b->itm_size && memcmp(a->itm_blob, b->itm_blob, a->itm_size) == 0, "items blob");
#undef CHK
    if (what) *what = w;
    return first;
}

const char *sg_zone_name(int area, int block)
{
    /* item.msgbnd fmg 19 (PlaceName) RUSSIAN, id = area*100 + block*10 [проверено 15.10, tools/cook_ui.load_fmgs] */
    if (area == 18 && block == 1) return "Северное Прибежище Нежити";   /* 1810 */
    if (area == 10 && block == 2) return "Храм Огня";                   /* 1020 */
    if (area == 10 && block == 1) return "Город Нежити";                /* 1010 */
    return "";
}

/* ---------------------------------------------------------------- progress bridge */

void sg_from_progress(SaveGame *s, const Progress *p)
{
    /* the same numbers prg_save_write stores (incl. "dying: save what the reload makes"), read back through a
       scratch Progress so the death rule lives in one place (src/progress) */
    static uint8_t blob[256];
    static Progress t;
    int n = prg_save_write(p, blob, sizeof(blob));
    prg_init(&t, &p->cfg);
    t.own_hp_max = *p->hp_max;
    if (n <= 0 || prg_save_read(&t, blob, n) != 0) t = *p, t.hp = p->hp, t.hp_max = p->hp_max;
    s->souls = t.souls;
    s->estus = t.estus;
    s->estus_max = p->cfg.estus_max;
    s->deaths = t.deaths;
    s->hp = *t.hp;
    s->hp_max = *p->hp_max;
    s->prg_flags = t.flags;
    s->stain_on = t.stain_on;
    memcpy(s->stain_pos, t.stain_pos, sizeof(s->stain_pos));
    s->stain_souls = t.stain_souls;
}

void sg_to_progress(const SaveGame *s, Progress *p)
{
    p->souls = s->souls;
    p->estus = s->estus > p->cfg.estus_max ? p->cfg.estus_max : s->estus;
    p->deaths = s->deaths;
    p->flags = s->prg_flags;
    p->stain_on = s->stain_on != 0;
    memcpy(p->stain_pos, s->stain_pos, sizeof(p->stain_pos));
    p->stain_souls = s->stain_souls;
    float hp = s->hp > *p->hp_max ? *p->hp_max : s->hp;
    *p->hp = hp > 0 ? hp : *p->hp_max;
}
