/*
 * combat.ashcb v1 - combat tables cooked by tools/cook_combat.py from the user's DSR copy (never in git).
 * Little-endian, every record is fixed size and naturally aligned, sections 16-aligned; the loader only points
 * into the file buffer (no copies). Plain C99, no PSP headers: the same code is unit-tested on the PC.
 *
 * Header: "ACMB", u32 version, u32 n_sec, u32 pad, then n_sec x {u32 off, u32 count} in CB_S_* order.
 * Frames are 30 Hz ticks of the animation ([start, end)); times in the runtime are seconds (frame = t * 30).
 */
#ifndef ASH_CBDATA_H
#define ASH_CBDATA_H

#include <stdint.h>

#define CB_VERSION 1

enum {
    CB_S_KIND, CB_S_ANIM, CB_S_STATE, CB_S_HIT, CB_S_EVT, CB_S_ATK, CB_S_DUMMY, CB_S_HURT, CB_S_NPC, CB_S_WEAPON,
    CB_S_THROW, CB_S_PLAYER, CB_S_COUNT
};

/* Reaction anims per kind (CbKind.react[]); 0 = the kind has none. Player ids are XX*10000+YYYY (a23_3000 -> 233000). */
enum {
    CB_R_DEATH, CB_R_HIT_S, CB_R_HIT_M, CB_R_HIT_L, CB_R_KNOCKDOWN, CB_R_PARRIED, CB_R_GUARD_HIT, CB_R_GUARD_HIT_L,
    CB_R_GUARD_BREAK, CB_R_ROLL, CB_R_BACKSTEP, CB_R_PARRY,
    CB_R_REPELLED,          /* weapon bounced off a shield (a00_2050 [версия], tools/cook_combat.py REACT) */
    CB_R_COUNT
};

typedef struct {             /* 84 B: one character model (c0000 player, c2500 hollow, c2230 Asylum Demon ...) */
    char name[8];
    uint16_t anim_first, n_anim, dummy_first, n_dummy, hurt_first, n_hurt;
    uint32_t react[16];
} CbKind;

typedef struct {             /* 24 B, sorted by anim_id inside a kind */
    uint32_t anim_id;
    uint16_t kind, len;      /* len = last event end, frames */
    uint16_t state_first, n_state, hit_first, n_hit, evt_first, n_evt;
    uint16_t stamina;        /* max BehaviorParam.stamina of its attack/throw behaviours, charged at start */
    uint16_t flags;
} CbAnim;

typedef struct {             /* 8 B: TAE ev.0 ChrActionFlag interval, already mapped to a CB_ST_* bit */
    uint16_t start, end;
    uint8_t bit, raw;        /* raw = DS1 FlagType */
    uint16_t pad;
} CbState;

enum { CB_HK_ATTACK = 1, CB_HK_BULLET = 2, CB_HK_THROW = 4, CB_HK_COMMON = 5 };
#define CB_ATTACK_PARRY 64    /* TAE ev.1 AttackType "Parry" (DSAnimStudio DS1 template) */
#define CB_ATTACK_PLUNGE 62

typedef struct {             /* 16 B: attack window (TAE ev.1/2/5/304) resolved to an AtkParam row */
    uint16_t start, end;
    uint16_t atk;            /* index into the atk table, 0xFFFF = unresolved */
    uint8_t kind;            /* CB_HK_* */
    uint8_t attack_type;     /* ev.1 AttackType: 0 standard, 62 plunge, 64 parry */
    uint16_t judge, stamina;
    int16_t bullet_dmy;      /* ev.2: dummy poly the bullet starts at */
    uint16_t bullet_r_cm;    /* ev.2: final radius of the (expanding) bullet hit sphere */
} CbHit;

typedef struct {             /* 12 B: other timed TAE events */
    uint16_t start, end, type;   /* 224 turn speed, 225 SP regen %, 226 knockback %, 236 root motion reduction, 66/67 SpEffect */
    uint16_t pad;
    float arg;
} CbEvt;

#define CB_ATK_IGNORE_GUARD 1
#define CB_ATK_NO_STAMINA 2
#define CB_ATK_IGNORE_IFRAMES 4
#define CB_ATK_NPC 8

typedef struct {             /* 88 B: AtkParam_Pc / AtkParam_Npc row */
    float radius[4];
    int16_t dmy_a[4], dmy_b[4];  /* dummy poly ids; capsule a-b if both >= 0 and differ, else sphere at a */
    uint8_t hit_type[4];
    uint16_t pct_phys, pct_mag, pct_fire, pct_thun, pct_stam, pct_guard_atk, pct_guard_break, pct_sa;  /* PC: % of weapon */
    uint16_t atk_phys, atk_mag, atk_fire, atk_thun, atk_stam, atk_sa, guard_atk_rate, guard_break_rate; /* NPC: absolute */
    float knockback, hitstop;    /* m, s */
    uint16_t throw_id;
    uint8_t dmg_level, atk_attr, sp_attr, hit_source, throw_flag, flags;
    uint32_t id;
} CbAtk;

typedef struct {             /* 48 B */
    int16_t id;
    uint8_t src;             /* 0 body, 1 right weapon model, 2 left weapon model */
    uint8_t pad;
    float local[3];          /* in `bone` space (mirrored, metres) */
    float model[3];          /* bind-pose model space: fallback when the runtime skeleton lacks `bone` */
    char bone[20];           /* "" = character root */
} CbDummy;

typedef struct {             /* 68 B: hurt capsule between two bone origins */
    char bone_a[20], bone_b[20];
    float radius;
    float model_a[3], model_b[3];   /* bind-pose model space fallback */
} CbHurt;

typedef struct {             /* 64 B: NpcParam with its permanent level SpEffect (spEffectID0..7) already applied */
    uint32_t id, beh_var;
    float hit_radius, hit_height, turn_vel;
    uint16_t hp, stamina, stamina_regen, poise;   /* hp/stamina x level multipliers */
    int16_t def_phys, def_mag, def_fire, def_thun, def_slash, def_strike, def_thrust;  /* flat defs x level */
    uint16_t souls, weight, pad;
    float atk_mult;          /* level SpEffect attack power multiplier (applies to its AtkParam_Npc rows) */
    float stam_dmg_mult;     /* OutgoingStaminaDamageMultiplier */
    float def_mult;          /* informational: already applied to def_* */
    uint32_t level_speffect; /* SpEffect id that carried the multipliers, 0 = none */
} CbNpc;

typedef struct {             /* 64 B: EquipParamWeapon (+ precomputed attack rating for the player's stats) */
    uint32_t id;
    uint16_t beh_var;
    uint8_t atk_cat, guard_level;
    uint16_t base_phys, base_mag, base_fire, base_thun, base_stam, base_sa;
    float ar[4];             /* attack rating phys/mag/fire/thun for the cooked player stats */
    float guard_cut[4];      /* % blocked */
    uint16_t stability;      /* GuardStaminaDefense */
    uint8_t parry_window, flags;   /* flags: 1 parry, 2 guard */
    uint16_t deflect_block, deflect_attack;
    float weight;
} CbWeapon;

typedef struct {             /* 48 B: ThrowParam (backstab / riposte) */
    uint32_t id;
    uint16_t atk_chr, def_chr;
    float dist, ang_min, ang_max, ang_to_def, y_up, y_low;
    uint32_t atk_anim, def_anim; /* atk_anim = YYYY of the attacker's weapon category */
    int16_t atk_dmy, def_dmy;
    uint8_t throw_type, atk_turns;
    uint16_t pad;
} CbThrow;

typedef struct {             /* 64 B: the player character (CharaInitParam class + equipment) */
    uint16_t level, vit, att, end, str, dex, intel, fai, luck, res;
    uint16_t hp, stamina;
    float stamina_regen;     /* per second [версия: wiki 45/s, not in PARAM] */
    uint32_t weapon_r, weapon_l;
    uint16_t poise, def_phys, def_mag, def_fire, def_thun, pad;
    float roll_stamina, backstep_stamina, guard_regen_mult;
    uint32_t pad2;
} CbPlayer;

typedef struct {
    const uint8_t *file;
    int size;
    const CbKind *kind;     int n_kind;
    const CbAnim *anim;     int n_anim;
    const CbState *state;   int n_state;
    const CbHit *hit;       int n_hit;
    const CbEvt *evt;       int n_evt;
    const CbAtk *atk;       int n_atk;
    const CbDummy *dummy;   int n_dummy;
    const CbHurt *hurt;     int n_hurt;
    const CbNpc *npc;       int n_npc;
    const CbWeapon *weapon; int n_weapon;
    const CbThrow *throws;  int n_throw;
    const CbPlayer *player;
} CbData;

/* 0 on success, <0 on a bad file (magic/version/section sizes). `file` must stay alive and be 4-aligned. */
int cb_data_load(CbData *d, const void *file, int size);
int cb_kind_find(const CbData *d, const char *name);
const CbAnim *cb_anim_find(const CbData *d, int kind, uint32_t anim_id);
const CbNpc *cb_npc_find(const CbData *d, uint32_t id);
const CbWeapon *cb_weapon_find(const CbData *d, uint32_t id);
/* ThrowParam row for attacker chr -> defender chr (model numbers, 0 = player) and kind (0 backstab, 8 riposte). */
const CbThrow *cb_throw_find(const CbData *d, int atk_chr, int def_chr, int riposte);

#endif
