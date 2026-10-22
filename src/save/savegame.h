/*
 * Ash save game (docs/save.md): what a "Продолжить" needs - character, inventory and equipment, progress (souls,
 * Estus, bloodstain), last bonfire and zone, event flags of every visited zone, settings. Plain C99, no PSP headers:
 * sg_pack / sg_unpack are tested natively (scripts/test-save.sh); storage (sceUtilitySavedata / file) is in
 * savestore.h.
 *
 * Bytes: the src/progress/save.h container ("ASHS", tagged parts, CRC-32 over everything). Parts written by
 * sg_pack, every field explicit little-endian (no struct dumps, so the layout does not depend on the compiler):
 *   META  u32 format (SG_FORMAT), u32 seq, u32 play_s, u8 area, u8 block, u16 pad, u32 bonfire, u32 flags,
 *         u64 time (sceRtc ticks or 0)
 *   CHR1  name[SG_NAME_BYTES] (UTF-8, 0-padded), u16 class_row, u8 gender, u8 gift, u8 hair, u8 pad[3],
 *         u16 level, u16 pad, u8 attr[8] (MA_* order of src/menu), i32 humanity, i32 souls, f32 hp, f32 hp_max,
 *         i32 estus, i32 estus_max, i32 deaths, u32 prg_flags
 *   STN1  i32 on, u8 area, u8 block, u16 pad, f32 pos[3], i32 souls
 *   INV1  u16 n_item, u16 n_equip(=SG_EQUIP_SLOTS), n_item x {u8 cat, u8 level, u16 count, u32 id},
 *         n_equip x i16 (index into the item list, -1 empty; MSLOT_* order of src/menu)
 *   SET1  i8 scheme, brightness, cam_speed, invert_x, invert_y, overlay, pad[2]
 *   ZEVT  u16 n, u16 pad, n x {u8 area, u8 block, u16 pad, u32 size, size bytes (4-aligned)} - ev_save_write blobs
 *   ITMS  the src/items inventory blob (itm_save_write: entries, equipment, active hands, quick slot) - written only
 *         when itm_size > 0. With src/items in the game this is THE inventory; INV1 is the items-DB-free form
 *         (tools, tests, a game without src/items) and may stay empty.
 * Unknown parts are skipped on load (newer saves stay readable as far as they share parts) [придумал].
 */
#ifndef ASH_SAVEGAME_H
#define ASH_SAVEGAME_H

#include <stdint.h>

#include "progress.h"

#define SG_FORMAT 1
#define SG_NAME_BYTES 64          /* = MenuChar.name: 16 code points of UTF-8 */
#define SG_MAX_ITEMS 64
#define SG_EQUIP_SLOTS 19         /* = MSLOT_COUNT (src/menu/menu.h) */
#define SG_MAX_ZONES 4            /* zones whose event flags are kept: Asylum, Firelink, Burg, Parish */
#define SG_EVT_BYTES 8192         /* per zone, = the game's ev_save_write buffer */
#define SG_ITM_BYTES 1024         /* src/items itm_save_write blob (itm_save_size() = 576 B on 15.10) */
#define SG_MAX_BYTES (2048 + SG_ITM_BYTES + 8 + SG_MAX_ZONES * (SG_EVT_BYTES + 8))   /* worst-case packed size */

enum { SG_CAT_WEAPON, SG_CAT_ARMOR, SG_CAT_RING, SG_CAT_GOODS };   /* EquipParamWeapon/Protector/Accessory/Goods */

typedef struct {
    uint8_t cat;                  /* SG_CAT_* */
    uint8_t level;                /* reinforcement +N */
    uint16_t count;               /* goods stack (1 for equipment) */
    uint32_t id;                  /* row id in that EquipParam table */
} SgItem;

typedef struct {
    uint8_t area, block;          /* m<area>_<block>: 18,1 = Northern Undead Asylum; 10,2 = Firelink Shrine */
    int size;
    uint8_t data[SG_EVT_BYTES];   /* ev_save_write() of that zone's Ev */
} SgZoneEvents;

typedef struct {
    /* META */
    uint32_t seq;                 /* write counter: the newest valid copy wins on load (set by savestore) */
    uint32_t play_s;              /* play time, s */
    uint8_t area, block;          /* zone the player is in */
    uint32_t bonfire;             /* last rested bonfire: event entity id (Ev.respawn) */
    uint32_t flags;               /* game-wide flags (free for the game) */
    uint64_t time;                /* wall clock of the write (sceRtcGetCurrentTick) or 0 */
    /* CHR1 */
    char name[SG_NAME_BYTES];
    uint16_t class_row;           /* CharaInitParam 3000..3009 */
    uint8_t gender, gift, hair;
    uint16_t level;
    uint8_t attr[8];              /* VIT ATT END STR DEX RES INT FTH */
    int32_t humanity, souls;
    float hp, hp_max;
    int32_t estus, estus_max, deaths;
    uint32_t prg_flags;           /* Progress.flags */
    /* STN1 */
    int32_t stain_on;
    uint8_t stain_area, stain_block;
    float stain_pos[3];
    int32_t stain_souls;
    /* INV1 */
    int n_item;
    SgItem item[SG_MAX_ITEMS];
    int16_t equip[SG_EQUIP_SLOTS];   /* index into item[], -1 = empty */
    /* SET1 */
    int8_t scheme, brightness, cam_speed, invert_x, invert_y, overlay;
    /* ITMS */
    int itm_size;                 /* 0 = none */
    uint8_t itm_blob[SG_ITM_BYTES];
    /* ZEVT */
    int n_zone;
    SgZoneEvents zone[SG_MAX_ZONES];
} SaveGame;

void sg_init(SaveGame *s);                       /* empty: nothing equipped, default settings, no zones */

/* Packs into buf (SG_MAX_BYTES is always enough). Returns bytes or <0. */
int sg_pack(const SaveGame *s, void *buf, int cap);
/* Unpacks a container. 0 ok; <0: -1 short, -2 magic/version, -3 CRC, -4 no META/CHR1, -5 newer format,
   -6 bad field. On error *s is reset to sg_init(). */
int sg_unpack(SaveGame *s, const void *buf, int len);

/* Event blob of a zone: returns the slot (added if missing, 0 if full) / the stored one (0 if none). */
SgZoneEvents *sg_zone_put(SaveGame *s, int area, int block);
const SgZoneEvents *sg_zone_get(const SaveGame *s, int area, int block);

/* Field-by-field compare: 0 equal, else the 1-based number of the first differing field group (for the test). */
int sg_compare(const SaveGame *a, const SaveGame *b, const char **what);

/* Zone name for the XMB / continue screen (UTF-8, DSR wording where known). */
const char *sg_zone_name(int area, int block);

/* ---- src/progress bridge (progress.h): souls, Estus, deaths, HP, bloodstain position/souls, Progress.flags.
   sg_from_progress saves what the reload will produce when the player is dying (as prg_save_write does). */
void sg_from_progress(SaveGame *s, const Progress *p);
void sg_to_progress(const SaveGame *s, Progress *p);   /* clamps Estus to cfg.estus_max, HP to *hp_max */

#endif
