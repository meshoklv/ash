/*
 * Ash combat module: DS1 combat rules on top of the cooked TAE/PARAM tables (cbdata.h).
 *
 * The game owns movement, animation playback and input; this module owns HP, stamina, poise, hit detection and
 * the rules (i-frames, guard, parry, riposte/backstab, stagger, death, knockback). Plain C99, no PSP headers, so
 * tests/combat runs it on the PC with the same data.
 *
 * Per frame (fixed 1/30 s step, the plan design notes 3.3):
 *   1. game advances its animations, then for every actor writes pos/yaw, bones (model space), anim_id/anim_t;
 *   2. combat_update(&world, dt) - regen, state flags, hit windows, damage; fills world.ev[] and actor->req_anim;
 *   3. game plays actor->req_anim (if != 0) as a forced transition, applies actor->push (knockback) through its
 *      collision, freezes an actor's animation while actor->hitstop > 0, reacts to events (sound, souls, UI).
 * Player actions: before starting an attack/roll/parry anim the game calls cb_begin_anim(); 0 = not allowed
 * (no stamina), the game keeps the current anim. R1 near a parried/back-turned enemy: cb_try_throw() first.
 *
 * Every rule that is not read from the data is tagged [версия] here and in docs/combat.md.
 */
#ifndef ASH_COMBAT_H
#define ASH_COMBAT_H

#include "cbdata.h"

#define CB_FPS 30.0f
#define CB_MAX_ACTORS 24
#define CB_MAX_EVENTS 32
#define CB_MAX_DUMMY 112        /* per kind (c0000 has 106 incl. weapon models) */
#define CB_MAX_HURT 16
#define CB_MAX_WIN 8            /* simultaneously tracked hit windows per attacker */

/* State bits (from TAE ChrActionFlag, see tools/cook_combat.py FLAG_BIT) */
enum {
    CB_ST_INVINCIBLE = 1u << 0, CB_ST_INV_EXC_THROW = 1u << 1, CB_ST_SUPER_ARMOR = 1u << 2,
    CB_ST_NO_SUPER_ARMOR = 1u << 3, CB_ST_PARRYABLE = 1u << 4, CB_ST_GUARD = 1u << 5, CB_ST_NO_TURN = 1u << 6,
    CB_ST_CANCEL_RH = 1u << 7, CB_ST_CANCEL_LH = 1u << 8, CB_ST_CANCEL_MOVE = 1u << 9, CB_ST_CANCEL_ROLL = 1u << 10,
    CB_ST_CANCEL_ANY = 1u << 11, CB_ST_CANCEL_ON_HIT = 1u << 12, CB_ST_CANCEL_PARRY_OK = 1u << 13,
    CB_ST_INPUT_RH = 1u << 14, CB_ST_INPUT_LH = 1u << 15, CB_ST_INPUT_ROLL = 1u << 16, CB_ST_DEATH = 1u << 17,
    CB_ST_NO_COLLISION = 1u << 18, CB_ST_CANCEL_GUARD = 1u << 19, CB_ST_NO_LOCKON = 1u << 20,
    CB_ST_PARRY_TIMING = 1u << 21, CB_ST_SA_BREAK = 1u << 22, CB_ST_COMMON_INPUT = 1u << 23,
    /* runtime-only bits */
    CB_ST_RIPOSTABLE = 1u << 28, /* parried: SpEffect 30 "throw condition" window */
    CB_ST_THROWN = 1u << 29,     /* victim of a riposte/backstab in progress */
};

enum {
    CB_EV_HIT = 1,          /* attacker hit victim: damage applied */
    CB_EV_BLOCK,            /* victim blocked with the shield: reduced damage + stamina loss */
    CB_EV_GUARD_BREAK,      /* block ran out of stamina */
    CB_EV_PARRY,            /* attacker (=parrier) parried victim; victim is ripostable for a while */
    CB_EV_STAGGER,          /* poise broken: victim plays a hit reaction */
    CB_EV_DEATH,            /* victim hp reached 0 */
    CB_EV_THROW,            /* riposte/backstab started (attacker, victim) */
    CB_EV_THROW_HIT,        /* riposte/backstab damage applied */
    CB_EV_INVULN,           /* hit landed during i-frames (no damage) - useful for debugging/tests */
    CB_EV_REPEL,            /* attacker's weapon bounced off victim's shield: attacker->req_anim = repelled anim */
};

/* Player attack actions -> anim ids (cb_attack_anim). Base YYYY of the one-handed set; two-handed = +1000 (a23_4xxx).
 * Roles of 3500/3600/3900 [версия]: by AtkParam_Pc row order (…520 running, …030 jump, …540 rolling) and TAE. */
enum {
    CB_ACT_R1 = 3000, CB_ACT_R1_2 = 3001, CB_ACT_R1_3 = 3002, CB_ACT_R2 = 3300, CB_ACT_R2_2 = 3301,
    CB_ACT_RUN_R1 = 3500, CB_ACT_RUN_R2 = 3600, CB_ACT_ROLL_R1 = 3900,
    CB_ACT_PLUNGE = 3800, CB_ACT_PLUNGE_FALL = 3801, CB_ACT_PLUNGE_LAND = 3810,
};

typedef struct {
    int type;
    int attacker, victim;   /* actor indices in the world */
    float damage;           /* hp removed (after guard) */
    float stamina;          /* stamina removed (block) */
    uint32_t anim;          /* reaction anim asked of the victim (also in victim->req_anim); REPEL: of the attacker */
    float pos[3];           /* contact point (world) */
    uint8_t atk_type;       /* HIT/BLOCK: TAE AttackType of the window (CB_ATTACK_PLUNGE 62 = plunge attack) */
    uint8_t pad[3];
} CbEvent;

typedef struct CbActor {
    /* ---- written by the game every frame ---- */
    float pos[3];
    float yaw;                  /* model = T(pos) * Ry(yaw); forward = (-sin yaw, 0, -cos yaw), as src/game */
    const float *bones;         /* n_bones 4x4 column-major (ScePspFMatrix4 layout), MODEL space; NULL = bind pose */
    int n_bones;
    uint32_t anim_id;           /* clip being played (id as in cbdata: player XX*10000+YYYY, NPC YYYY) */
    float anim_t, anim_t_prev;  /* seconds into the clip now / at the previous combat_update */
    uint8_t guard_input;        /* player is holding block (shield up, not attacking) */
    uint8_t ai_guard;           /* NPC guarding (NpcParam guard) - unused in S1 */

    /* ---- set up once (cb_actor_init_player / cb_actor_init_npc + cb_actor_bind) ---- */
    int kind;                   /* index in CbData.kind */
    int chr;                    /* model number: 0 player, 2500, 2230 ... */
    int team;                   /* hits only cross teams */
    const CbNpc *npc;           /* NULL for the player */
    const CbWeapon *wep_r, *wep_l;
    int16_t dmy_bone[CB_MAX_DUMMY]; /* per kind dummy -> runtime bone index (-1 = use bind-pose model position) */
    int16_t hurt_bone[CB_MAX_HURT][2];

    /* ---- combat state (owned by the module, readable by the game/UI) ---- */
    float hp, hp_max, sp, sp_max, sp_regen;
    float poise, poise_max, poise_timer;
    float def[4];               /* flat defense phys/mag/fire/thun */
    uint32_t flags;             /* CB_ST_* of the current anim at anim_t (+ runtime bits) */
    uint32_t req_anim;          /* != 0: play this now (reaction); the game clears it after starting the anim */
    float hitstop;              /* s: freeze this actor's anim */
    float push[3];              /* knockback velocity (m/s, world) for push_t seconds */
    float push_t;
    float ripostable_t;         /* s left in which a riposte is possible */
    int throw_partner;          /* other actor of a riposte/backstab, -1 none */
    float throw_atk_t;          /* attacker: anim time at which the throw damage lands */
    const CbAtk *throw_atk;
    uint8_t dead, throw_done;
    /* hit bookkeeping */
    uint32_t win_anim;          /* anim the windows below belong to */
    uint32_t win_hit[CB_MAX_WIN];  /* bit per victim index: already hit by window k of this anim */
    float prev_tip[CB_MAX_WIN][4][3]; /* previous frame shape end points (swept test) */
    uint8_t prev_valid[CB_MAX_WIN];
    float regen_block;          /* s: no stamina regen (after an action) [версия] */
    uint32_t plunge_hit;        /* bit per victim already hit by the current plunge (3800 -> 3801 -> 3810: one hit) */
} CbActor;

typedef struct {
    const CbData *data;
    CbActor *actor[CB_MAX_ACTORS];
    int n_actor;
    CbEvent ev[CB_MAX_EVENTS];
    int n_ev;
    float time;
    int debug_draw;             /* combat_debug_shapes() fills shapes when set */
    int ev_ext;                 /* events pushed between updates (cb_try_throw): kept into the next update's list */
} CbWorld;

/* ---- setup ---- */
void cb_world_init(CbWorld *w, const CbData *d);
int cb_world_add(CbWorld *w, CbActor *a);  /* -> index */
void cb_actor_init_player(CbActor *a, const CbData *d);
int cb_actor_init_npc(CbActor *a, const CbData *d, uint32_t npc_param_id, const char *kind_name);
/* Resolve dummy/hurt bone names against the runtime skeleton (names of the actor's .ashc bones). */
typedef int (*CbBoneLookup)(void *ctx, const char *name);
void cb_actor_bind(CbActor *a, const CbData *d, CbBoneLookup lookup, void *ctx);

/* ---- per frame ---- */
void combat_update(CbWorld *w, float dt);
/* The game is about to start `anim_id` on actor `a` by its own decision (input/AI). Charges stamina for actions
 * that cost it (attacks: BehaviorParam; roll/backstep: CbPlayer [версия]). Returns 0 if not allowed: the player has
 * no stamina (DS1: an action needs stamina > 0, the bar may go negative... clamped at 0 here [версия]). */
int cb_begin_anim(CbWorld *w, CbActor *a, uint32_t anim_id);
/* R1 pressed by actor `ai`: tries riposte (target ripostable, in front) then backstab (behind). On success sets
 * both req_anims (attacker: weapon category * 10000 + ThrowParam.atk_anim), returns the victim index, else -1. */
int cb_try_throw(CbWorld *w, int ai);

/* Anim id of a player attack action (CB_ACT_*) with the actor's right weapon: category * 10000 + YYYY
 * (+1000 two-handed). 0 if the data has no such anim (then the game keeps its current anim).
 * Plunge: the game plays CB_ACT_PLUNGE when R1 is pressed while falling, CB_ACT_PLUNGE_FALL (loop) until it lands,
 * then CB_ACT_PLUNGE_LAND - all three through cb_begin_anim (the landing charges its stamina). Their windows are
 * AttackType 62; the module lets one plunge hit each victim once, and the HIT event carries atk_type 62. */
uint32_t cb_attack_anim(const CbWorld *w, const CbActor *a, int action, int two_handed);

/* ---- queries / helpers ---- */
uint32_t cb_anim_flags(const CbData *d, const CbAnim *an, float t);
const CbAnim *cb_actor_anim(const CbWorld *w, const CbActor *a);
/* World position of dummy poly `id` (src 0 body, 1 right weapon, 2 left weapon); 0 if the kind has none. */
int cb_dummy_world(const CbWorld *w, const CbActor *a, int id, int src, float out[3]);
/* DS1 damage of one element: community approximation (tl.net "Dark Souls Stats I", 2013) [версия]:
 *   atk <  def: 0.4*atk^3/def^2 - 0.09*atk^2/def + 0.1*atk
 *   atk >= def: atk - 0.79*def*exp(-0.27*def/atk) */
float cb_damage_formula(float atk, float def);
/* Damage the attack would deal (before guard), elements summed; *poise_dmg / *stam_dmg optional. */
float cb_attack_damage(const CbWorld *w, const CbActor *atk_actor, const CbAtk *atk, const CbActor *victim,
                       float *poise_dmg, float *stam_dmg);
float cb_capsule_dist(const float a0[3], const float a1[3], const float b0[3], const float b1[3]);

/* Debug: up to max capsules (a, b, radius, kind 0 hurt / 1 hit) for the game to draw. */
typedef struct { float a[3], b[3], r; int kind; } CbShape;
int combat_debug_shapes(const CbWorld *w, CbShape *out, int max);

#endif
