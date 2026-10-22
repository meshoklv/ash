/*
 * Ash progression: Estus, souls, bloodstain, death -> "ВЫ ПОГИБЛИ" -> bonfire, bonfire rest (sit / menu / stand),
 * banners and fades for the HUD, save blob. Plain C99 (no PSP headers): tested natively (src/progress/test).
 *
 * Who owns what (no duplication with the other modules):
 *   combat (src/combat)  - HP / stamina / damage / death detection. Progress only reads and refills HP through the
 *                          pointers given to prg_bind_hp() (the player's CbActor hp / hp_max).
 *   events (src/event)   - bonfire objects, lit flags, respawn point, enemy respawn (reload), prompts, boss bar
 *                          commands. Progress reacts to its commands (EV_CMD_REST, _RELOAD, _KILL, _KILL_BOSS,
 *                          _BONFIRE_LIT, _BANNER) and asks for a reload (PRG_REQ_RELOAD) after the death screen.
 *   game (src/game)      - plays the clips progress asks for (Progress.anim, names in prg_anim_name[] =
 *                          clips of knight_prg.ashanim), blocks movement while prg_locked(), writes the save.
 *
 * Per frame (fixed 1/30 s):
 * Hooks (prg_rest_begin, prg_player_died ...) may be called at any time; their requests come out of the next
 * prg_tick together with the tick's own.
 *     PrgInput in = { player pos, item (Estus) pressed, menu up/down/confirm/cancel, busy (rolling/attacking) };
 *     prg_tick(&p, &in, dt);
 *     if (p.req & PRG_REQ_ANIM)   play clip prg_anim_name[p.anim] (loop if p.anim_loop), cancel the current action;
 *     if (p.req & PRG_REQ_RELOAD) ev_player_died(ev)  (-> EV_CMD_RELOAD -> prg_on_reload + move to ev_respawn);
 *     if (p.req & PRG_REQ_SAVE)   write the save (prg_save_write + ev_save_write) - src/progress/save.h;
 *     if (p.req & PRG_REQ_STAND_DONE) the player is free again after resting;
 *   combat CB_EV_DEATH of the player -> prg_player_died(pos);  of an enemy -> prg_add_souls(npc->souls);
 *   player staggered (CB_EV_STAGGER / HIT) while drinking -> prg_interrupt().
 * HUD: prg_hud.h fills a HudFrame from Progress (+ the boss values the game knows).
 *
 * Every number that is not from the game's data is tagged in docs/progress-ui.md ([проверено]/[версия]/[придумал]).
 */
#ifndef ASH_PROGRESS_H
#define ASH_PROGRESS_H

#include <stdint.h>

enum {
    PRG_ANIM_NONE = 0, PRG_ANIM_DRINK_IN, PRG_ANIM_DRINK, PRG_ANIM_DRINK_OUT, PRG_ANIM_DRINK_EMPTY,
    PRG_ANIM_SIT, PRG_ANIM_SIT_LOOP, PRG_ANIM_STAND, PRG_ANIM_DEATH, PRG_ANIM_COUNT
};
extern const char *const prg_anim_name[PRG_ANIM_COUNT];   /* clip names in knight_prg.ashanim ("" for NONE) */

enum { PRG_PLAY, PRG_DRINK, PRG_REST, PRG_DYING, PRG_LOADING };

enum {                         /* Progress.req: requests to the game, valid until the next prg_tick */
    PRG_REQ_ANIM = 1u << 0,    /* play p.anim now (forced) */
    PRG_REQ_RELOAD = 1u << 1,  /* death screen is black: reload the map state, respawn at the bonfire */
    PRG_REQ_SAVE = 1u << 2,    /* autosave now */
    PRG_REQ_HEALED = 1u << 3,  /* Estus HP added this frame (sound / effect) */
    PRG_REQ_STAND_DONE = 1u << 4,
    PRG_REQ_SOULS_BACK = 1u << 5,
};

enum { PRG_MENU_MAX = 4 };

typedef struct {
    int estus_max;             /* charges after a rest */
    float estus_heal;          /* HP per drink: SpEffect 3020 (Estus +0) HPPointsLost -300 */
    float heal_time;           /* s over which the heal is added */
    float heal_at;             /* s into the "drink" clip when the heal starts */
    float stain_radius;        /* m: touching the bloodstain */
    float banner_in, banner_hold, banner_out;
    float death_banner_at, death_black_at, death_black_len, loading_time, fade_in;
    float autosave_souls_every;  /* s: min gap between "souls changed" autosaves */
} PrgCfg;

typedef struct {
    float pos[3];              /* player feet, PSP space */
    int item;                  /* Estus button pressed (edge) */
    int up, down, confirm, cancel;  /* menu (edges) */
    int busy;                  /* player in an action that blocks item use (roll, attack, stagger, fall) */
} PrgInput;

typedef struct {
    PrgCfg cfg;
    float *hp, *hp_max;        /* bound to the player's combat actor; own storage until prg_bind_hp() */
    float own_hp, own_hp_max;
    /* ---- persistent (save blob) */
    int32_t souls, estus, deaths;
    int32_t stain_on;
    float stain_pos[3];
    int32_t stain_souls;
    uint32_t flags;            /* free for the game (e.g. tutorial hints shown) */
    /* ---- state */
    int phase, sub;
    float t;                   /* s in the current phase / sub-phase */
    float clip_len[PRG_ANIM_COUNT];
    float heal_left;           /* HP still to add (gradual Estus heal) */
    int heal_started;
    float death_pos[3];
    int menu_cursor, menu_n;
    int menu_item[PRG_MENU_MAX];      /* S_* string ids */
    uint8_t menu_enabled[PRG_MENU_MAX];
    float since_save;
    int save_pending;
    uint32_t req_pending;
    int anim_pending, anim_loop_pending;
    /* ---- outputs (valid after prg_tick) */
    uint32_t req;
    int anim, anim_loop;
    int banner;                /* IMG_* id (src/ui/uistr.h) or -1 */
    float banner_t;            /* s since the banner was raised */
    int banner_sticky;         /* death banner: stays until the reload */
    float fade;                /* 0..1 black over everything */
    int loading;               /* loading screen on */
    int toast;                 /* S_* id or -1 */
    float toast_t;
    int32_t souls_gain;        /* accumulated recent gain for the "+N" over the counter */
    float souls_gain_t;
} Progress;

void prg_default_cfg(PrgCfg *c);
void prg_init(Progress *p, const PrgCfg *c);     /* new game: full Estus, 0 souls, no stain */
void prg_bind_hp(Progress *p, float *hp, float *hp_max);
void prg_set_clip_len(Progress *p, int anim, float seconds);

void prg_tick(Progress *p, const PrgInput *in, float dt);
int prg_locked(const Progress *p);               /* 1: gameplay input and movement off (drink/rest/death) */
int prg_resting(const Progress *p);

/* ---- hooks from the game / events / combat */
int prg_try_estus(Progress *p);                  /* also called by prg_tick on in->item; 1 = started */
void prg_interrupt(Progress *p);                 /* hit while drinking: the charge is spent, the rest of the heal lost */
void prg_player_died(Progress *p, const float pos[3]);   /* pos = where the bloodstain goes (last safe ground) */
void prg_on_reload(Progress *p);                 /* EV_CMD_RELOAD (rest or after death): HP + Estus refill, stain */
void prg_rest_begin(Progress *p);                /* EV_CMD_REST: sit down, bonfire menu */
void prg_add_souls(Progress *p, int n);          /* enemy killed (NpcParam.getSoul) / EV_CMD_KILL a / boss */
void prg_banner(Progress *p, int img);           /* IMG_VICTORY on EV_CMD_KILL_BOSS, IMG_BONFIRE_LIT, IMG_AREA_* */
void prg_toast(Progress *p, int str);

/* ---- save blob (little-endian, versioned; container + CRC in save.h) */
#define PRG_SAVE_VERSION 1
int prg_save_size(void);
int prg_save_write(const Progress *p, void *buf, int cap);   /* bytes written or -1 */
int prg_save_read(Progress *p, const void *buf, int len);    /* 0 ok, <0 bad (state untouched) */

#endif
