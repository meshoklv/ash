/* `.ashevt` v1 on-disk layout (tools/cook_events.py writes it, docs/events.md §3). Little-endian, 4-aligned,
 * offsets from the start of the file. Loaded in place: the file buffer must stay alive while the Ev runs. */
#ifndef ASH_EVFILE_H
#define ASH_EVFILE_H

#include <stdint.h>

#define EVF_VERSION 1

enum { EVS_INST, EVS_CODE, EVS_ENT, EVS_TREAS, EVS_LOT, EVS_LOTITEM, EVS_STR, EVS_POOL, EVS_ROOT, EVS_OBJACT,
       EVS_DROP, EVS_COUNT };

typedef struct { uint32_t n, off; } EvfSec;
typedef struct { char magic[4]; uint32_t version; EvfSec sec[EVS_COUNT]; } EvfHeader;

typedef struct { uint32_t event_id; uint16_t slot; uint8_t rest, flags; uint32_t code_off, n; } EvfInst;

enum { EVK_REGION = 1, EVK_OBJECT, EVK_CHR, EVK_COLL, EVK_SPAWN, EVK_VFX, EVK_POINT };
enum { EVSH_POINT, EVSH_SPHERE, EVSH_CYLINDER, EVSH_BOX };

/* PSP space (x mirrored vs FromSoft, yaw negated). Regions: box dim = width (local x), depth (local z), height,
 * pos = centre of the bottom face; cylinder dim = radius, height (pos = bottom centre); sphere dim[0] = radius.
 * Objects: model = number of oNNNN; dim = model-specific (fog/door span x0, x1, height; ladder bottom z, top y,
 * top z). Characters: model = cNNNN, extra = NpcParam, extra2 = NpcThinkParam. Bonfire objects: extra = spawn
 * point entity. VFX: model = vfx id, extra = attached object entity. */
typedef struct {
    uint32_t id;
    uint8_t kind, shape;
    uint16_t str;
    float pos[3], yaw, dim[3];
    uint32_t model, extra, extra2;
} EvfEnt;

typedef struct { uint32_t obj, lot[2]; float pos[3]; uint8_t hidden, pad[3]; uint32_t extra; } EvfTreasure;
typedef struct { uint32_t lot_id, flag; uint16_t first, n; } EvfLot;
typedef struct { uint32_t item; uint16_t count, str; } EvfLotItem;      /* item = cat << 28 | id */
typedef struct { uint32_t key, off; } EvfStr;
typedef struct {
    uint32_t id, obj, prompt, fail, need;
    uint8_t need_type, pad[3];               /* 0 none, 1 item key in `need`, 2 special effect on the player */
    uint32_t auto_flag, done_flag;
} EvfObjAct;
typedef struct { uint32_t chr, lot; } EvfDrop;

#endif
