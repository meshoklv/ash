/*
 * Ash items: the item database (items.ashit from tools/cook_items.py), inventory with stacks and limits, DS1
 * equipment slots (2 right, 2 left, arrows/bolts 2+2, armour 4, rings 2, quick items 5), equipping with recomputed
 * weight / load / poise / defence / attack rating, item use (Estus, souls, humanity, heal, throw, keys), loot (ItemLot
 * -> items, "Получено: ..."), starting inventory per class (CharaInitParam) and a save blob.
 * Plain C99, no PSP headers: tested natively (src/items/test/run.sh). The menu bridge (items_menu.c) is separate.
 *
 * Item key = cat << 28 | id, the same key as src/event (EvfLotItem.item, EV_CMD_ITEM c.a, objact `need`):
 *   cat 0 EquipParamWeapon, 1 EquipParamProtector, 2 EquipParamAccessory, 4 EquipParamGoods.
 *
 * Who owns what:
 *   progress (src/progress) - Estus charges (Progress.estus), souls, humanity is ours (Inv.humanity) unless the game
 *                             moves it; itm_use() only reports what to do (ITM_USE_ESTUS -> prg_try_estus,
 *                             ITM_USE_SOULS -> prg_add_souls ...), it never touches Progress.
 *   events (src/event)      - which lot is awarded when, lot ItemFlags, door checks. The game forwards EV_CMD_ITEM
 *                             to itm_add() (or awards the whole lot with itm_award_lot()) and may answer door checks
 *                             with itm_has().
 *   combat (src/combat)     - uses the numbers of ItmStats (attack rating, defence, poise, load ratio -> roll type).
 * Numbers that are not in PARAM are tagged in docs/items.md ([проверено] / [версия] / [придумал]).
 */
#ifndef ASH_ITEMS_H
#define ASH_ITEMS_H

#include <stdint.h>

/* ------------------------------------------------------------------ cooked file (items.ashit, little-endian) */

enum { ITM_CAT_WEAPON = 0, ITM_CAT_ARMOR = 1, ITM_CAT_RING = 2, ITM_CAT_GOODS = 4 };
#define ITM_KEY(cat, id) (((uint32_t)(cat) << 28) | (uint32_t)(id))
#define ITM_KEY_CAT(k) ((int)((k) >> 28))
#define ITM_KEY_ID(k) ((int)((k) & 0x0FFFFFFFu))

/* ItmRec.kind */
enum { ITM_WK_MELEE, ITM_WK_SHIELD, ITM_WK_BOW, ITM_WK_XBOW, ITM_WK_CATALYST, ITM_WK_TALISMAN, ITM_WK_FLAME,
       ITM_WK_ARROW, ITM_WK_BOLT };                                   /* weapons (by id range [версия]) */
enum { ITM_AK_HEAD, ITM_AK_BODY, ITM_AK_ARMS, ITM_AK_LEGS };          /* armour */
enum { ITM_GK_CONSUMABLE, ITM_GK_KEY, ITM_GK_MATERIAL, ITM_GK_ESTUS, ITM_GK_OTHER };   /* goods */
/* ItmRec.flags */
enum { ITM_F_CONSUMED = 1, ITM_F_QUICK = 2, ITM_F_RIGHT = 4, ITM_F_LEFT = 8, ITM_F_TWOHAND = 16, ITM_F_UNIQUE = 32 };
/* ItmRec.use */
enum { ITM_USE_NONE, ITM_USE_ESTUS, ITM_USE_SOULS, ITM_USE_HUMANITY, ITM_USE_HEAL, ITM_USE_THROW, ITM_USE_SPEFFECT,
       ITM_USE_KEY };

/* ItmRec.v[] by category */
enum { ITM_WV_ATK = 0, ITM_WV_SCALE = 4, ITM_WV_REQ = 8, ITM_WV_GUARD = 12, ITM_WV_STABILITY = 16, ITM_WV_GRAPH = 17 };
enum { ITM_AV_DEF = 0, ITM_AV_STRIKE = 4, ITM_AV_SLASH = 5, ITM_AV_THRUST = 6, ITM_AV_POISE = 7, ITM_AV_BLEED = 8,
       ITM_AV_POISON = 9, ITM_AV_CURSE = 10 };
enum { ITM_RV_LOAD_PCT = 0, ITM_RV_HP_PCT = 1, ITM_RV_SP_PCT = 2, ITM_RV_POISE = 3, ITM_RV_DEF = 4, ITM_RV_SP_REGEN = 8 };
enum { ITM_GV_ANIM = 0, ITM_GV_REPLACE = 1 };

typedef struct {
    char magic[4];               /* "ASIT" */
    uint32_t version;            /* 1 */
    uint32_t n_item, off_item, n_lot, off_lot, n_lotitem, off_lotitem, n_class, off_class, n_start, off_start;
    uint32_t n_graph, off_graph, off_str, str_size;
    uint32_t s_got, s_used, s_full, pad;   /* "Получено: \x01", "Использовано: \x01" (\x01 = the name), "Нельзя ..." */
} ItmHdr;                        /* 80 B */

typedef struct {
    uint32_t key;
    uint32_t name, desc;         /* offsets in the string pool (0 = "") */
    uint16_t icon, max_hold;     /* icon = image index in items.ashui (0xffff none); max stack */
    uint8_t cat, kind, flags, use;
    float weight;
    int32_t use_val;             /* souls / HP / humanity / Bullet id / SpEffect id (ring: its SpEffect) */
    int16_t v[18];               /* ITM_WV_* / ITM_AV_* / ITM_RV_* / ITM_GV_* */
} ItmRec;                        /* 64 B, sorted by key */

typedef struct { uint32_t lot_id, flag; uint16_t first, n; } ItmLot;            /* 12 B, sorted by lot_id */
typedef struct { uint32_t key; uint16_t count, chance; } ItmLotItem;            /* key id 0 = "nothing" */
typedef struct {
    uint16_t row; uint8_t level, humanity;
    uint8_t attr[8];             /* VIT ATT END STR DEX RES INT FTH (= src/menu MA_*), 0 for gifts */
    uint16_t first, n;           /* ItmStart range */
} ItmClass;                      /* 16 B: rows 3000..3009 (classes), 2400..2408 (gifts) */
typedef struct { uint32_t key; uint16_t count; uint8_t slot, asylum; } ItmStart; /* slot ITM_SLOT_* or 0xff;
                                    asylum: 0 = full start only, 1 = both, 2 = Asylum start only */
typedef struct { uint16_t id, pad; float x[5], y[5], e[5]; } ItmGraph;          /* CalcCorrectGraph, 64 B */

typedef struct {
    const uint8_t *file;
    int size;
    const ItmHdr *h;
    const ItmRec *item;
    const ItmLot *lot;
    const ItmLotItem *lotitem;
    const ItmClass *cls;
    const ItmStart *start;
    const ItmGraph *graph;
    const char *str;
} ItmDb;

int itm_db_load(ItmDb *db, const void *file, int size);    /* 0 ok, <0 bad file; the buffer must outlive db */
const ItmRec *itm_find(const ItmDb *db, uint32_t key);      /* binary search, 0 if unknown */
const char *itm_str(const ItmDb *db, uint32_t off);
const char *itm_name(const ItmDb *db, uint32_t key);        /* "" if unknown */
const ItmLot *itm_find_lot(const ItmDb *db, uint32_t lot_id);
const ItmClass *itm_find_class(const ItmDb *db, int row);

/* ------------------------------------------------------------------ inventory + equipment */

enum {                           /* = src/menu MSLOT_* (DS1 equipment screen order) */
    ITM_SLOT_R1, ITM_SLOT_R2, ITM_SLOT_L1, ITM_SLOT_L2, ITM_SLOT_ARROW1, ITM_SLOT_ARROW2, ITM_SLOT_BOLT1,
    ITM_SLOT_BOLT2, ITM_SLOT_HEAD, ITM_SLOT_BODY, ITM_SLOT_ARMS, ITM_SLOT_LEGS, ITM_SLOT_RING1, ITM_SLOT_RING2,
    ITM_SLOT_Q1, ITM_SLOT_Q2, ITM_SLOT_Q3, ITM_SLOT_Q4, ITM_SLOT_Q5, ITM_SLOT_COUNT
};

#define ITM_INV_MAX 64           /* inventory entries [придумал: в DS1 почти без предела; 64 хватает Акту 1] */

typedef struct { uint32_t key; uint16_t count; uint16_t order; } ItmEntry;   /* order: pickup order (menu sort) */

typedef struct {
    ItmEntry e[ITM_INV_MAX];
    int n;
    int8_t eq[ITM_SLOT_COUNT];   /* inventory index per slot, -1 = empty */
    int8_t r_active, l_active;   /* 0/1: R1 or R2, L1 or L2 in hand */
    int8_t two_hand;             /* right weapon held in both hands (STR x1.5) */
    int8_t quick;                /* current quick slot 0..4 */
    int32_t humanity;
    uint16_t next_order;
    const int32_t *estus;        /* bound to Progress.estus (charges shown in the menu); 0 = no flask count */
} Inv;

enum {                           /* itm_equip / itm_can_equip results */
    ITM_OK = 0, ITM_ERR_INDEX = -1, ITM_ERR_SLOT = -2, ITM_ERR_FULL = -3, ITM_ERR_UNKNOWN = -4, ITM_ERR_USE = -5
};

void itm_inv_init(Inv *inv);
/* New game: class row 3000..3009, gift row 2400..2408 (or 0), asylum = 1: the DS1 Asylum start (hilt, no shield, no
   Estus; class weapons lie near bonfire 2), 0: the full CharaInitParam kit (Estus x5 [версия]). Equips what the
   class wears. Returns entries added. */
int itm_new_game(Inv *inv, const ItmDb *db, int class_row, int gift_row, int asylum);

/* Adds count of key: goods stack up to max_hold, weapons / armour / rings one entry each. Returns how many were
   actually added (less than count = no room / stack full -> "Нельзя унести больше"). Unknown keys are refused. */
int itm_add(Inv *inv, const ItmDb *db, uint32_t key, int count);
int itm_remove(Inv *inv, uint32_t key, int count);         /* from the latest entries first; unequips; returns n */
int itm_remove_at(Inv *inv, int idx, int count);
int itm_count(const Inv *inv, uint32_t key);
int itm_has(const Inv *inv, uint32_t key);                 /* door / objact key check (src/event `need`) */
int itm_find_entry(const Inv *inv, uint32_t key);          /* first inventory index or -1 */

int itm_slot_accepts(const ItmDb *db, int slot, const ItmRec *r);
int itm_can_equip(const Inv *inv, const ItmDb *db, int slot, int idx);   /* ITM_OK or ITM_ERR_* */
/* Puts entry idx into slot. If idx is already in another slot of the same group it moves there (DS1 swap); a ring of
   the same key cannot be worn twice. */
int itm_equip(Inv *inv, const ItmDb *db, int slot, int idx);
void itm_unequip(Inv *inv, int slot);
int itm_equipped(const Inv *inv, int slot);                /* inventory index or -1 */
uint32_t itm_equipped_key(const Inv *inv, int slot);       /* 0 if empty */
int itm_slot_of(const Inv *inv, int idx);                  /* first slot holding idx or -1 */
/* Inventory entries that may go into slot (menu "choose item" list): writes up to max indices, returns count. */
int itm_candidates(const Inv *inv, const ItmDb *db, int slot, int *out, int max);
uint32_t itm_right_weapon(const Inv *inv);                 /* key in the active right hand, 0 = bare fist */
uint32_t itm_left_weapon(const Inv *inv);
void itm_switch_right(Inv *inv);                           /* -> : R1 <-> R2 (skips an empty one) */
void itm_switch_left(Inv *inv);                            /* <- : L1 <-> L2 */

/* quick items (DS1 v: next quick slot) */
int itm_quick_next(Inv *inv);                              /* moves to the next non-empty quick slot; returns slot */
int itm_quick_entry(const Inv *inv);                       /* inventory index in the current quick slot or -1 */

/* ------------------------------------------------------------------ stats */

enum { ITM_ROLL_FAST, ITM_ROLL_MID, ITM_ROLL_FAT, ITM_ROLL_OVER };

typedef struct {
    float hp_max, sp_max;        /* from VIT / END tables [версия] x ring multipliers */
    float load, load_max, load_ratio;
    int roll;                    /* ITM_ROLL_*: <=25 % fast, <=50 % mid, <=100 % fat, above - overloaded (DS1) */
    float poise;
    float def[4];                /* phys mag fire thun: armour + rings (no level/attribute base) [версия] */
    float def_strike, def_slash, def_thrust;
    float res_bleed, res_poison, res_curse;
    float ar_r[4], ar_l[4];      /* attack rating of the weapon in hand (CalcCorrectGraph scaling) */
    int req_ok_r, req_ok_l;      /* requirements met (else penalty, no scaling [версия]) */
    float guard_l[4];            /* % blocked by the left-hand item */
    float stability_l;
} ItmStats;

/* attr = VIT ATT END STR DEX RES INT FTH (src/menu MA_* order) */
void itm_calc(const Inv *inv, const ItmDb *db, const uint8_t attr[8], ItmStats *out);
float itm_weapon_ar(const ItmDb *db, const ItmRec *w, const uint8_t attr[8], int two_hand, float ar[4], int *req_ok);
float itm_correct(const ItmGraph *g, float stat);          /* CalcCorrectGraph -> percent 0..100 */
float itm_hp_for_vit(int vit);
float itm_sp_for_end(int end);

/* ------------------------------------------------------------------ use */

typedef struct {
    int kind;                    /* ITM_USE_* */
    int value;                   /* souls / HP / humanity / Bullet / SpEffect */
    uint32_t key;
    int anim;                    /* EquipParamGoods.UseAnimation (the game's clip choice) */
    int consumed;                /* 1 = one was removed from the inventory */
} ItmUse;

/* Uses inventory entry idx. Estus: nothing is removed (the game calls prg_try_estus, which owns the charges).
   Souls / humanity / heal / throw / other SpEffect goods: one is removed when the item is consumed on use.
   Humanity is added to inv->humanity here. Keys and materials cannot be used: returns ITM_ERR_USE. */
int itm_use(Inv *inv, const ItmDb *db, int idx, ItmUse *out);
int itm_use_quick(Inv *inv, const ItmDb *db, ItmUse *out);

/* ------------------------------------------------------------------ loot */

typedef struct { uint32_t key; int count, added; } ItmGot;

/* Awards ItemLot lot_id. A lot with an empty slot that has ChancePoints (enemy drop) gives ONE item by weighted pick
   with *rng (xorshift32, never 0); any other lot gives all its items (treasure) [версия]. The lot's ItemFlag is the
   caller's business (src/event). Returns entries written to got (0 = lot unknown or "nothing" was rolled). */
int itm_award_lot(Inv *inv, const ItmDb *db, uint32_t lot_id, uint32_t *rng, ItmGot *got, int max_got);
/* "Получено: Длинный меч", "Получено: Огненная бомба x6, ..." (FMG prefix); "Нельзя унести больше" when nothing fit. */
int itm_got_text(const ItmDb *db, const ItmGot *got, int n, char *buf, int cap);
int itm_used_text(const ItmDb *db, uint32_t key, char *buf, int cap);   /* "Использовано: ..." (door keys) */

/* ------------------------------------------------------------------ save blob (little-endian, versioned) */
#define ITM_SAVE_VERSION 1
int itm_save_size(void);
int itm_save_write(const Inv *inv, void *buf, int cap);    /* bytes or -1 */
int itm_save_read(Inv *inv, const ItmDb *db, const void *buf, int len);   /* 0 ok, <0 bad (inv untouched) */

#endif
