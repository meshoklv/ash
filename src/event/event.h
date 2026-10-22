/*
 * Ash event interpreter: runs the map event script cooked from DSR EMEVD (tools/cook_events.py -> `.ashevt`) with
 * DS1 semantics (condition groups MAIN/AND/OR, skip/return lines, waits, event flags, instances per slot).
 * Plain C99, no PSP or game dependencies: the game is reached only through EvHost (queries + one command sink),
 * so the same file runs on the PSP and in the native test (src/event/test). Docs: docs/events.md.
 *
 * Per frame (game side):
 *     EvInput in = { player pos/yaw, interact pressed, floor part name, ... };
 *     ev_tick(ev, dt, &in);                  // runs events + native interactables (loot, doors, ladders, bonfires)
 *     ev_push_out(ev, pos, r, ignore_fog);   // closed doors/gates/fog walls as blockers for the player capsule
 *     draw ev_prompt(ev) ; draw objects via ev_obj_count/ev_obj_get (enabled/open/destroyed)
 * On player death: ev_player_died(ev); the next tick reloads (volatile flags cleared, events restarted) and
 * sends EV_CMD_RELOAD; respawn the player at ev_respawn(ev). Bonfire rest does the same (EV_CMD_REST first).
 * Save: ev_save_size/ev_save_write on EV_CMD_SAVE; ev_save_read before ev_start on boot.
 */
#ifndef ASH_EVENT_H
#define ASH_EVENT_H

#include <stdint.h>
#include "evfile.h"
#include "evops.h"

#define EV_PLAYER 10000u

/* ---- host interface */
enum {                       /* EvHost.query `what` */
    EVQ_HP_RATIO,            /* -> 0..1 (characters the host does not know: 1) */
    EVQ_DEAD,                /* -> 1 dead (death animation finished), 0 alive */
    EVQ_ATTACKED,            /* id attacked by arg this frame -> 1/0 */
};

enum {                       /* EvCmd.op — what the game has to do. Return value of cmd(): seconds to block */
    EV_CMD_LOG,              /* text: debug line */
    EV_CMD_CHR_SET,          /* id, a = EV_CHR_*, b = value */
    EV_CMD_KILL,             /* id, a = award souls */
    EV_CMD_CHR_ANIM,         /* id, a = anim, b = loop -> duration (s) */
    EV_CMD_MOVE,             /* id <- pos, yaw (warp; fog: anim 7410 follows) */
    EV_CMD_FACE,             /* id faces pos */
    EV_CMD_STANDBY,          /* id, a = standby anim (-1 reset) */
    EV_CMD_SPEFFECT,         /* id, a = effect, b = add */
    EV_CMD_NEST,             /* id, pos = nest point */
    EV_CMD_AI_CMD,           /* id, a = command, b = slot */
    EV_CMD_BOSS_BAR,         /* id, a = on, b = name id, text = name */
    EV_CMD_BOSS_START,
    EV_CMD_KILL_BOSS,        /* id = game area (= boss chr) -> souls + "VICTORY ACHIEVED" */
    EV_CMD_OBJ_ANIM,         /* id, a = anim, f[0] = -1 instantly (end of animation) -> duration */
    EV_CMD_OBJ_STATE,        /* id, a = enabled, b = destroyed */
    EV_CMD_VFX,              /* id = vfx entity, a = on, b = vfx id, pos */
    EV_CMD_MAPCOL,           /* id = collision entity, a = enabled, text = part name */
    EV_CMD_DIALOG,           /* a = text id, text */
    EV_CMD_CUTSCENE,         /* id = cutscene, a = title text id, text = title -> seconds (fade) */
    EV_CMD_SOUND,            /* id, a = on (BGM / sound event) */
    EV_CMD_SFX,              /* id = anchor, a = type, b = sound id */
    EV_CMD_ITEM,             /* a = item key (cat << 28 | id), b = count, text = name: "got item" */
    EV_CMD_OBJACT,           /* id = obj, a = objact id, b = 1 success / 0 fail, text = fail message */
    EV_CMD_LADDER,           /* id = obj, a = 1 up / 0 down, pos/yaw = where the player ends -> seconds */
    EV_CMD_BONFIRE_LIT,      /* id = obj */
    EV_CMD_REST,             /* id = obj -> seconds (fade); a reload follows */
    EV_CMD_RELOAD,           /* events restarted: respawn enemies, refill HP/estus, player to ev_respawn() */
    EV_CMD_SAVE,             /* write ev_save_write() (+ the game's own state) */
    EV_CMD_BALL,             /* id = ball obj, a = target obj, pos -> pos2 (f[0] speed, f[1] radius) -> seconds */
    EV_CMD_BOMB,             /* id = thrower, a = target chr, pos -> pos2: throw one firebomb */
    EV_CMD_BANNER,           /* a = text id, text */
    EV_CMD_END_DEMO,         /* text: "Конец демо" — the script stops here */
    EV_CMD_COUNT
};

typedef struct {
    int op;
    uint32_t id;
    int32_t a, b;
    float pos[3], pos2[3], yaw;
    float f[2];
    const char *text;
} EvCmd;

typedef struct {
    void *user;
    float (*query)(void *user, int what, uint32_t id, uint32_t arg);
    /* position (feet) + yaw of a character; 0 if the game does not have it (then regions test false) */
    int (*chr_pos)(void *user, uint32_t id, float pos[3], float *yaw);
    float (*cmd)(void *user, const EvCmd *c);
} EvHost;

typedef struct {
    float pos[3], yaw;           /* player, PSP space */
    int interact;                /* action button pressed this frame (edge) */
    const char *floor_part;      /* collision part name under the feet (Pcol part name, e.g. "h0006B1"), or 0 */
} EvInput;

/* ---- runtime */
#define EV_MAX_FLAGS 1024
#define EV_MAX_ITEMS 64
#define EV_MAX_LADDERS 8
#define EV_MAX_BONFIRES 8
#define EV_MAX_SPEFF 8

typedef struct {
    uint16_t pc, status;          /* status: 0 idle, 1 running, 2 done */
    uint8_t in_cond, yield;
    uint16_t cond_start;
    int8_t grp[16], last[16];     /* group g in -7..8 -> g + 7; -1 = empty */
    float t, wait;                /* time since (re)start; seconds left of a blocking op */
    int32_t wait_frames, counter; /* counter: multi-step ops (BOMB, BALL) */
    uint32_t cpc, cw;             /* cache: word offset of instruction cpc */
} EvRun;

typedef struct { uint8_t enabled, destroyed, open, act_enabled; int16_t anim; uint8_t vfx_on, pad; } EvObjRt;
typedef struct { uint32_t start_flag, stop_flag, obj; } EvLadder;
typedef struct { uint32_t flag, obj; float dist; } EvBonfire;
typedef struct { uint32_t key; uint16_t count, pad; } EvItem;

typedef struct {
    /* file */
    const EvfHeader *h;
    const EvfInst *inst; const uint32_t *code; const EvfEnt *ent; const EvfTreasure *treas;
    const EvfLot *lot; const EvfLotItem *lotitem; const EvfStr *str; const char *pool; const uint32_t *root;
    const EvfObjAct *objact; const EvfDrop *drop;
    uint32_t n_inst, n_ent, n_treas, n_lot, n_str, n_root, n_objact, n_drop;
    EvHost host;
    /* state */
    EvRun *run;                   /* n_inst */
    EvObjRt *obj;                 /* n_ent (only objects / vfx / collisions use it) */
    uint8_t *treas_on;            /* n_treas */
    uint8_t *objact_pulse;        /* n_objact */
    uint32_t flags[EV_MAX_FLAGS]; /* sorted ids of flags that are ON */
    int n_flags;
    EvItem items[EV_MAX_ITEMS];
    int n_items;
    EvLadder ladder[EV_MAX_LADDERS]; int n_ladder;
    EvBonfire bonfire[EV_MAX_BONFIRES]; int n_bonfire;
    struct { uint32_t id; float t; } speff[EV_MAX_SPEFF];
    uint32_t respawn;             /* spawn point entity */
    EvInput in;
    float time; uint32_t frame;
    /* action button arbitration: the prompt chosen last frame wins the press this frame */
    uint32_t prompt_key, prompt_text_id; float prompt_dist;
    uint32_t cand_key, cand_text_id; float cand_dist;
    uint32_t fire_key;
    int pending_reload, demo_end, ninstr_last;
} Ev;

/* file = whole .ashevt (4-aligned, kept alive); mem = ev_mem_size() bytes of zeroed scratch for runtime arrays */
int ev_mem_size(const uint8_t *file, int size);
int ev_load(Ev *ev, const uint8_t *file, int size, void *mem, const EvHost *host);
void ev_start(Ev *ev);                         /* first load of the map: runs preconstructor + constructor */
void ev_tick(Ev *ev, float dt, const EvInput *in);
void ev_player_died(Ev *ev);                   /* reload on the next tick (after the game's death screen) */

/* push the player circle (xz, radius r) out of closed doors / gates / fog walls / intact breakable walls */
int ev_push_out(const Ev *ev, float pos[3], float r, int ignore_fog);

/* flags (DS1 ids). Volatile flags (1xxx5xxx) are cleared on reload and not saved. */
int ev_flag(const Ev *ev, uint32_t id);
void ev_set_flag(Ev *ev, uint32_t id, int on);

/* lookups */
const EvfEnt *ev_ent(const Ev *ev, uint32_t id);
const char *ev_text(const Ev *ev, uint32_t key);  /* key: text id, 1<<28|npc name, 2<<28|cat<<24|item */
const char *ev_prompt(const Ev *ev);               /* prompt to show this frame or 0 */
int ev_respawn(const Ev *ev, float pos[3], float *yaw);
int ev_has_item(const Ev *ev, uint32_t key);

/* objects for drawing: index 0..n_ent-1, returns 0 if that entity is not an object */
typedef struct { uint32_t id, model; float pos[3], yaw, dim[3]; int enabled, open, destroyed, anim; } EvObjInfo;
int ev_obj_get(const Ev *ev, int i, EvObjInfo *o);
/* treasures on the floor that can be picked up now */
int ev_treasure_get(const Ev *ev, int i, float pos[3]);

/* save blob: flags (non-volatile), respawn, inventory */
int ev_save_size(const Ev *ev);
int ev_save_write(const Ev *ev, void *buf, int cap);
int ev_save_read(Ev *ev, const void *buf, int len);

/* region test in PSP space */
int ev_in_region(const EvfEnt *r, const float p[3]);

#endif
