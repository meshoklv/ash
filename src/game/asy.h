/*
 * Ash "Asylum": the first location of the S1 slice as one game (docs/asylum.md).
 * Northern Undead Asylum m18_01 from the cell to the crow: zone world + Havok collision (src/world), events of the
 * map (src/event), DS1 combat rules (src/combat), hollows on the original DSR Lua AI (src/ai), the Asylum Demon
 * (src/boss, Lua 223200_battle), Estus / souls / bloodstain / bonfire rest / save (src/progress) and the HUD (src/ui).
 *
 * This header is the platform-independent part: the whole simulation (game_step) runs the same on the PSP
 * (asy_main.c: sceGu render, pad, frame dumps) and natively (tests/native/asy_native.c: no render, fast scenario
 * runs). Coordinates: PSP space (x mirrored vs FromSoft), model = T(pos) * Ry(yaw), forward = (-sin yaw, -cos yaw).
 */
#ifndef ASH_ASY_H
#define ASH_ASY_H

#include <psptypes.h>
#include <pspctrl.h>
#include <pspgu.h>

#include "aihost.h"
#include "anim.h"
#include "boss.h"
#include "boss_ai.h"
#include "camera.h"
#include "chr.h"
#include "combat.h"
#include "event.h"
#include "hud.h"
#include "input.h"
#include "items.h"
#include "pcol.h"
#include "progress.h"
#include "ui.h"
#include "world.h"

#define ASY_DT (1.0f / 30.0f)
#define ASY_PI 3.14159265f
#define ASY_MAX_EN 12
#define ASY_MAX_BOMB 4
#define ASY_DEMON_BODY_R 1.7f   /* the knight is pushed out of this cylinder around the demon [версия] */

typedef struct { const AnimSet *set; int idx; } ClipRef;

/* skinned, animated body with a 2-clip crossfade (knight or hollow) */
typedef struct {
    const Chr *chr;
    ClipRef cur, prev;
    float t, pt, blend, blend_len;
    int hold;                 /* 1: clamp at the end even if the clip loops (death pose) */
    BonePose __attribute__((aligned(16))) pa[ANIM_MAX_BONES], pb[ANIM_MAX_BONES];
    ScePspFMatrix4 __attribute__((aligned(16))) world[ANIM_MAX_BONES], skin[ANIM_MAX_BONES];
} Body;

typedef enum {
    P_IDLE, P_WALK, P_JOG, P_RUN, P_ROLL, P_BACKSTEP, P_ATTACK, P_GUARD, P_PARRY, P_THROW, P_REACT, P_PRG,
    P_FOG, P_FALL, P_PLUNGE, P_LADDER, P_COUNT
} PSt;
extern const char *const asy_pst_name[P_COUNT];

enum { DIR_F, DIR_B, DIR_L, DIR_R, DIR_COUNT };

typedef struct {
    float pos[3], yaw, vy;
    int grounded, floor_tri;
    float fall_from;          /* y where the current fall started */
    float carry[3];
    float guard_walk;         /* m/s while walking with the shield up */           /* horizontal step of the last grounded frame (momentum while falling) */
    PSt st;
    int dir, redirect;
    float move_dir, move_heading, speed_frac;
    int buffered;             /* ACT_* bit buffered (0.25 s), 0 none */
    float buffer_left;
    int basis_valid;
    float basis_yaw, basis_dir;
    int combo;                /* 0,1,2: attack, attack2, attack3 */
    float fog_left;           /* s of the 7410 fog walk */
    float safe_pos[3];        /* last grounded spot (bloodstain) */
    int dead;
} Player;

typedef struct {
    u32 entity;
    int zchr;                 /* index in World.chr */
    Body body;
    float pos[3], yaw, vy;
    int floor_tri;
    float home[3], home_yaw;
    CbActor act;
    int cbi;                  /* index in the combat world */
    AiAgent ai;
    int ai_ok;
    int enabled, ai_on, alerted, dying, dead;
    float dead_t, react_t;
    int ez;                   /* EzState being played, -1 */
    float ez_left;
    float mv_fwd, mv_side;    /* AI move request of this tick */
    int mv_walk, mv_face, mv_set;
    int attacked;             /* hit by the player this frame (EVQ_ATTACKED) */
} Enemy;

typedef struct { int on; float p0[3], p1[3], t, len; } Bomb;

typedef struct {
    int on;
    int obj;                  /* World obj index */
    float p0[3], p1[3], t, len, radius;
    int hit;
} Ball;

/* autopilot (headless has no pad): see asy_auto.c */
typedef struct {
    int scenario;             /* ASY_SC_* */
    int step;                 /* index in the scenario script */
    float step_t;             /* s in the current step */
    float stuck_t, best_d;
    int warps, stuck_total;
    int falls;                /* fell off the route (a drop > 2.5 m outside the plunge) */
    float ground[3];          /* last grounded position (fall detection) */
    int was_air;
    float appr_best, appr_t;   /* approach to fight_target: closest distance, s without progress */
    int ignore_en;            /* hollow the bot cannot reach (on a ledge): left alone until ignore_until */
    float ignore_until;
    int done;                 /* script finished */
    int fight_target;         /* enemy index or 100 boss, -1 */
    float fight_t;
    int lock_wanted;
    u32 prev_buttons;
    int prompt_tries;
    int boss_attempts;
    int phase;                /* per-op sub state */
    float x, z;               /* scratch */
    char note[64];
} Auto;

enum { ASY_SC_NONE, ASY_SC_ROUTE, ASY_SC_HOLLOW, ASY_SC_DEATH, ASY_SC_BOSS, ASY_SC_COUNT };
enum { ASY_ZONE_ASYLUM, ASY_ZONE_FIRELINK };   /* Game.zone: m18_01 / m10_02 (full game, ds_main.c) */

typedef struct Game {
    /* data */
    World W;
    Pcol C;
    int has_col;
    Ev ev;
    void *ev_mem;
    CbData cbd;
    CbWorld cbw;
    Progress prg;
    UiRes ui;
    int ui_ok;
    Hud hud;
    AiHost *ai;
    /* knight */
    Chr kchr;
    AnimSet kbase, kcmb, kprg;
    int has_cmb, has_prg;
    ClipRef kclip[P_COUNT][DIR_COUNT];
    ClipRef k_attack[3], k_heavy, k_guard, k_parry, k_death, k_walk;
    Player pl;
    Body pb;
    CbActor pact;
    int pidx;
    u32 p_anim_id;
    /* hollows */
    Chr hchr;
    AnimSet hanim;
    int has_hollow;
    int h_idle, h_walk, h_run, h_atk1, h_atk2, h_hit, h_death;
    Enemy en[ASY_MAX_EN];
    int n_en;
    /* boss */
    Boss B;
    BossAi BA;
    int has_boss, boss_lua, boss_enabled, boss_ai_on, boss_hidden;
    CbActor bact;
    int bidx;
    float boss_vy;
    u32 boss_last_swing;
    int boss_bar;
    /* camera, input, lock-on */
    CamCfg ccfg;
    CamState cam;
    const CamRow *row;
    InputState inp;
    AshInput in;
    u32 raw_buttons, raw_pressed;
    int lock;                 /* -1 none, 0..n_en-1 hollow, 100 boss */
    float lock_pt[3];
    /* events / overlays */
    int *ev_obj_world;        /* per ev entity: World obj index or -1 */
    const char *toast_text;
    char toast_buf[128];
    float toast_t;
    float ev_fade, ev_fade_t, ev_fade_len;   /* black fades asked by events (cutscene, ladder) */
    int end_demo;
    float end_t;
    Bomb bomb[ASY_MAX_BOMB];
    Ball ball;
    int victory;
    /* bookkeeping */
    u32 frame;                /* simulation steps */
    float time;
    int deaths, kills, reloads, saves, rests, parries, ripostes, blocks, rolls_iframe, hits_given, hits_taken;
    int plunges, estus_used, items, bonfires_lit;
    int verbose;
    int camlog;               /* demo.txt opt "camlog": camera trace every 15 frames in the boss fight */
    int doorfight;            /* demo.txt opt "doorfight": the boss fight opens with the knight standing still at
                                 the back door 1811110 for 60 s (no death), a regression test of the door push */
    float cam_dbg[4];         /* last camera cast per source: map, objects, decor, demon (camlog) */
    unsigned cam_casts;       /* camera sphere casts so far (status line: casts per frame) */
    Auto au;
    int autopilot;
    /* effects asked by the simulation this step (drawn by asy_main.c: sparks, dust); cleared by the consumer */
    float fx_pos[8][3], fx_dir[8][3];
    int fx_kind[8];           /* 1 hit spark, 2 parry/block spark, 3 roll dust */
    int n_fx;
    /* debug */
    int ninstr;
    /* zone (full game "Ash: Dark Souls", ds_main.c): Asylum with its events, or a zone without an .ashevt (Firelink)
       whose interactions are the game's own (zone_step). Buffers kept to free them on a zone change. */
    int zone;                 /* ASY_ZONE_* */
    int has_ev;               /* events loaded (Asylum); 0 = zone_step runs the zone */
    u8 *f_pak, *f_ev, *f_hollow[3], *f_boss[4];
    float respawn[4];         /* zones without events: bonfire respawn point x y z yaw */
    void (*zone_step)(struct Game *g, float dt);   /* per step, after combat (Firelink: bonfire, NPC, loot) */
    const char *prompt_text;  /* HUD prompt of zone_step (NULL = the events' prompt) */
    float void_y;             /* below this the player fell into the void (m18_01: 150) */
    /* items (src/items): inventory, equipment, stats of the chosen class; has_items = items.ashit loaded */
    int has_items, class_row, level;
    ItmDb idb;
    Inv inv;
    ItmStats ist;
    u8 attr[8];
    u32 rng;
    u8 *f_items, *f_body;
    CbWeapon wep_r_ovr, wep_l_ovr;   /* the longsword/knight shield combat records with the class weapon numbers */
    /* hollow combat clips (src/combat 6.4: stagger, knockdown, repelled) */
    AnimSet hcmb;
    int has_hcmb;
    ClipRef k_plunge[3], k_run_r1, k_run_r2, k_roll_r1;   /* plunge / plunge_fall / plunge_land, running, rolling */
    int plunge_done;          /* the landing check already dealt the plunge damage (no second hit from the module) */
    /* hooks of the full game (ds_main.c) */
    int ext_menu;             /* 1: bonfire menu and "ВЫ ПОГИБЛИ" drawn by src/menu; the HUD's own are hidden */
    int ext_leave_bonfire;    /* set by the main when the bonfire menu closes: stand up */
    void (*save_fn)(struct Game *g, const char *why);   /* 0 = ash.sav through src/progress/save.h (as before) */
} Game;

/* loading: a file reader (cwd/host0/ms0 on the PSP, a staging dir natively). Returns size or <0; *out 64-aligned */
typedef int (*AsyLoadFn)(const char *name, u8 **out);
int game_load(Game *g, AsyLoadFn load);
void game_start(Game *g, int scenario);
/* One fixed 1/30 s step. raw = pad (ignored when the autopilot drives). */
void game_step(Game *g, const RawPad *raw);
/* PSP: pose every visible body (after game_step, before drawing) */
void game_pose(Game *g);

/* autopilot */
void auto_init(Game *g, int scenario);
void auto_pad(Game *g, RawPad *out);
const char *auto_scenario_name(int sc);

/* full game: items / class (ds_main.c) and zone change */
int game_load_common(Game *g, AsyLoadFn load);       /* game_load = common + Asylum */
int game_zone_load_asylum(Game *g, AsyLoadFn load);
int game_set_body(Game *g, AsyLoadFn load, const char *ashc);   /* player model (class armour), same 30-bone rig */
void game_set_class(Game *g, int class_row, int gift_row, const u8 attr[8], int level);   /* new game: class kit */
void game_apply_equipment(Game *g);    /* itm_calc -> combat actor (HP/stamina max, weapon AR, shield, defence) */
void game_use_item(Game *g, int inv_idx);   /* menu "use" / quick item */
void game_zone_unload(Game *g);        /* frees the zone pak, events, hollows, the demon, Lua */
/* loads another zone pak into g->W / g->C (no events, no enemies); the player stands at pos/yaw */
int game_zone_load_plain(Game *g, AsyLoadFn load, const char *pak, int zone, const float pos[4]);
void game_place_player(Game *g, const float pos[3], float yaw);
void game_reload_now(Game *g);         /* death / rest reload of a zone without events */

/* helpers used by both sides */
float asy_wrap_pi(float a);
void asy_fs(float x, float y, float z, float out[3]);   /* FromSoft -> PSP space */
float asy_flat_dist(const float a[3], const float b[3]);
int asy_has_estus(const Game *g);
void asy_log(Game *g, const char *fmt, ...);

#endif
