/*
 * Asylum Demon (c2232, m18_01) - boss runtime for the PSP: data (.ashboss), animation + root motion, attack hit
 * volumes from TAE/AtkParam, a C state machine built from FromSoftware's own AI script 223200_battle (distances,
 * weights, combos - docs/boss.md section 3), scripted moments (drop from the ceiling, balcony phase), HP bar with
 * the Russian name from FMG.
 *
 * Space: PSP world space (x mirrored against FromSoft data, like every cooked asset). Model matrix of the boss =
 * T(pos) * Ry(yaw); the model faces -Z, so forward = (-sin yaw, 0, -cos yaw); yaw grows counter-clockwise
 * (from above), as in src/game.
 *
 * Depends on src/game/anim.h and chr.h only (no combat/AI headers): the game wires it, every 1/30 s step:
 *     boss_tick(&boss, &world, DT);           world = player position + optional collision callback
 *     n = boss_hits(&boss, hits, 8);          active attack volumes (world space) -> combat (one hit per swing)
 *     boss_take_hit(&boss, dmg, poise, f);    the player's hit landed on boss_hurt_capsule()
 *     boss_event(&boss, BOSS_EV_...);         the player started an attack / uses an item (script interrupts)
 * and once per frame boss_draw() (3D pass, model matrix set inside) and boss_hud_draw() (2D pass).
 * If src/combat runs the boss as a CbActor instead: bones = boss.world (model space), anim_id = boss_anim_id(),
 * anim_t = boss.t; then boss_hits() is not used. Details: docs/boss.md.
 */
#ifndef ASH_BOSS_H
#define ASH_BOSS_H

#include <psptypes.h>
#include <pspgu.h>

#include "anim.h"
#include "chr.h"

/* ------------------------------------------------------------------ .ashboss v1 (tools/cook_boss.py) */

typedef struct {
    char magic[4];
    u32 version, n_atk, n_win, n_dmy, hp;
    float turn_deg_s, radius, height;
    u32 off_atk, off_win, off_dmy, n_reg, off_reg;
    char name[40]; /* UTF-8, FMG NPC_name 2232 */
} BossFileHdr;

typedef struct { s8 dmy0, dmy1; u16 pad; float radius; } BossFileHit; /* dmy = index into the dummy table */
typedef struct {
    u16 phys, stam, poise, impact;
    float knock;
    u16 flags;  /* BATK_* */
    u8 n_hit, pad;
    BossFileHit hit[3];
    u32 behavior, pad2;
} BossFileAtk;
typedef struct { u8 clip, kind, f0, f1; s16 value; u16 pad; } BossFileWin;
typedef struct { s16 id, bone; float pos[3]; } BossFileDmy;
typedef struct { s32 id; u32 shape; float pos[3], size[3]; } BossFileReg;

enum { BW_HIT = 1, BW_TURN = 2, BW_IFRAME = 3, BW_CANCEL = 4, BW_SUPERARMOR = 5 };
enum { BATK_IGNORE_GUARD = 1, BATK_BLAST = 2 };

/* MSB m18_01 regions (entity ids) the AI script and the scripted moments use */
enum {
    BREG_BALCONY = 1812350,     /* player on the balcony above the hall */
    BREG_BALCONY_IN1 = 1812351, /* inner balcony (left / right) */
    BREG_BALCONY_IN2 = 1812352,
    BREG_ARENA = 1812990,       /* hall (BGM region): outside it the demon goes back (Act16) */
    BREG_BATTLE = 1812996,      /* battle start region on the hall floor */
    BREG_HOME = 1812301,        /* "balcony aim" point (SetEventMoveTarget in Act16) */
    BREG_SKY = 1812305,         /* the drop point in the air (entrance clip ev9060) */
    BREG_START = 1812300,       /* where the demon stands after the drop (MSB c2232_0000 spot) */
    BREG_EXIT = 1812320,        /* boss room exit (the door the player escapes through) */
    BREG_ENTRY_TOP = 1812998,   /* upper entrance of the second fight */
};

/* ------------------------------------------------------------------ world interface (filled by the game) */

typedef struct {
    float player[3];       /* player position (feet), PSP space */
    float player_radius;   /* capsule radius (GetDist is surface distance [версия]) */
    int player_dead;       /* 1: the demon stops thinking */
    /* optional: move pos by (dx, dz) against the map collision; NULL = clamp to the arena box (BREG_ARENA) */
    void (*collide)(void *ctx, float pos[3], float dx, float dz);
    void *ctx;
} BossWorld;

typedef struct {
    float a[3], b[3];      /* capsule ends, world space (a == b: sphere) */
    float radius;
    int atk;               /* row of the attack table: boss_atk(b, atk) */
    u32 swing;             /* unique per window activation: a target is hit once per swing */
} BossHit;

enum { BOSS_HIT_NORMAL = 0, BOSS_HIT_PLUNGE = 1 };
enum { BOSS_EV_PLAYER_ATTACK = 1, BOSS_EV_PLAYER_ITEM = 2 };
enum { BOSS_AI_FSM = 0, BOSS_AI_EXTERNAL = 1 };

typedef enum {
    BS_INACTIVE, /* not in the level yet (before the drop) */
    BS_WAIT,     /* placed, waiting for the player (not engaged) */
    BS_ENTRANCE, /* ev9060: the drop from the ceiling */
    BS_IDLE,     /* thinking / between sub-goals */
    BS_MOVE,     /* approach, sideway, go home */
    BS_TURN,     /* turning on the spot */
    BS_ACTION,   /* playing an EzState (attack, step) */
    BS_DEATH,
    BS_DEAD,
} BossState;

/* sub-goals (the AI script's AddSubGoal calls the state machine executes) */
enum { SG_APPROACH = 1, SG_ATTACK, SG_COMBO, SG_NONSPIN, SG_SIDEWAY, SG_GOTO, SG_WAIT, SG_TURN };
typedef struct { u8 kind, walk_only, pad[2]; s16 ez; s16 arg; float dist, life; } BossStep;

#define BOSS_MAX_PLAN 6

typedef struct {
    /* data (pointers into the caller's buffers) */
    Chr chr;
    AnimSet anim;
    u8 *file;
    const BossFileHdr *hdr;
    const BossFileAtk *atk;
    const BossFileWin *win;
    const BossFileDmy *dmy;
    const BossFileReg *reg;
    int clip_idle, clip_walk, clip_walk_l, clip_walk_r, clip_run, clip_turn[4], clip_death, clip_entrance;
    int clip_of_ez[32];   /* ez 3000..3023 -> clip; 701 in ez_step */
    int clip_step;
    int dmy_rfoot, dmy_lfoot; /* observe spheres of the script: dummy 3 and 4 */
    int lock_bone;
    const u16 *name_px;   /* boss_name.ashtx (PSM 4444) */
    int name_w, name_h, name_tw;

    /* state */
    BossState st;
    float pos[3], yaw;
    int hp, hp_max;
    float hp_trail, hp_trail_wait;
    int clip, prev_clip;
    float t, prev_t, blend, t_prev_tick;
    int ez;                 /* EzState being played (attack id) or -1 */
    u32 swing_base;
    int ai_mode;
    BossStep plan[BOSS_MAX_PLAN];
    int n_plan, plan_i;
    float step_t;           /* time in the current sub-goal */
    int well_space;         /* GetWellSpace_Odds of the act in progress */
    int number[2];          /* ai:SetNumber(0/1): balcony phase counters */
    int last_act;
    u32 rng;
    int think_count, engaged, visible;
    float dist, angle;      /* surface distance to the player (m), angle (rad, + = to the left) */
    float player[3];
    char log[96];           /* last decision (test overlay / stdout) */

    BonePose pose_a[ANIM_MAX_BONES] __attribute__((aligned(16)));
    BonePose pose_b[ANIM_MAX_BONES] __attribute__((aligned(16)));
    ScePspFMatrix4 world[ANIM_MAX_BONES] __attribute__((aligned(16))); /* bone matrices, MODEL space */
    ScePspFMatrix4 skin[ANIM_MAX_BONES] __attribute__((aligned(16)));
} Boss;

/* Load from memory blocks (64-byte aligned, kept alive by the caller); name_file may be NULL. 0 = ok. */
int boss_load(Boss *b, u8 *chr_file, int chr_size, u8 *anim_file, int anim_size, u8 *boss_file, int boss_size,
              u8 *name_file, int name_size);
/* Places the boss (pos NULL = MSB spot BREG_START, facing the hall), full HP, waiting for the player.
 * Call again after the player's death (DS1: the boss comes back at full HP). */
void boss_reset(Boss *b, const float *pos, float yaw);
/* First encounter: the demon drops from the ceiling (ev9060: -28.2 m root motion from BREG_SKY). */
void boss_enter_drop(Boss *b);
void boss_tick(Boss *b, const BossWorld *w, float dt);

int boss_hits(const Boss *b, BossHit *out, int max);
const BossFileAtk *boss_atk(const Boss *b, int row);
/* Body for the player's hits: vertical capsule (NpcParam HitRadius / HitHeight). */
void boss_hurt_capsule(const Boss *b, float a[3], float b_[3], float *radius);
/* Applies damage, returns the HP left. The demon never flinches (no damage clips in c2232.anibnd [проверено]). */
int boss_take_hit(Boss *b, int damage, int poise_damage, int flags);
void boss_event(Boss *b, int ev);

/* External AI (Lua host): play EzState `ez`; returns its length (s), < 0 if unknown. */
float boss_request_ez(Boss *b, int ez);
void boss_ext_move(Boss *b, float forward, float side, int walk, int face_target, float dt);

u32 boss_anim_id(const Boss *b);      /* anim id of the current clip (3000, 9060...), for src/combat */
int boss_alive(const Boss *b);        /* HP > 0 */
int boss_defeated(const Boss *b);     /* death clip finished: souls, fog wall off */
int boss_iframes(const Boss *b);      /* TAE flag 8 window active (entrance) */
void boss_lock_point(const Boss *b, float out[3]);
int boss_in_region(const Boss *b, int id, const float p[3]);
int boss_region_center(const Boss *b, int id, float out[3]);
int boss_dummy_world(const Boss *b, int dmy_index, float out[3]);

void boss_draw(Boss *b);              /* 3D pass, GU_LIGHTING expected on; keeps the GU_MODEL matrix stack */
void boss_hud_draw(const Boss *b);    /* 2D pass: name + HP bar (DS1 layout, bottom of the screen) */
/* Debug: projects hit volumes to the screen as small squares (2D pass). */
void boss_debug_draw(const Boss *b, const ScePspFMatrix4 *vp);

#endif
