/*
 * Ash HUD in the DS1 layout, 480x272: HP (red) and stamina (green) bars top-left with the yellow "lost HP" trail,
 * the d-pad cross of quick slots bottom-left (Estus in the bottom slot with its count), souls bottom-right (+gain
 * that rolls into the counter), boss name + bar bottom-centre, action prompt centre-bottom, toast, banners
 * ("ВЫ ПОГИБЛИ" / "ВРАГ ПОВЕРЖЕН" / "КОСТЕР ЗАЖЖЕН" / area name - DSR textures, font fallback), bonfire menu,
 * loading screen, fade to black.
 *
 * The game fills a HudFrame every frame (src/progress/prg_hud.h does the progress part), then:
 *     hud_tick(&hud, &f, dt);   // cosmetic animation only (trails, souls roll)
 *     ui_begin(); hud_draw(&hud, &f); ui_end();
 * World markers (bloodstain): hud_draw_stain() before hud_draw, with the frame's view-projection.
 */
#ifndef ASH_HUD_H
#define ASH_HUD_H

#include "ui.h"

typedef struct {
    /* player */
    float hp, hp_max, sp, sp_max;
    int estus, estus_max;          /* estus_max < 0: no Estus yet (slot empty) */
    int souls, souls_gain;         /* gain: recent "+N" shown over the counter (0 = none) */
    float souls_gain_t;
    /* boss (EV_CMD_BOSS_BAR / Boss) */
    int boss_on;
    float boss_hp, boss_hp_max;
    const char *boss_name;
    /* centre prompt (ev_prompt() / bonfire / fog ...) - NULL none */
    const char *prompt;
    /* overlays */
    int banner;                    /* IMG_* or -1 */
    float banner_a, banner_scale;  /* alpha 0..1, scale (DS1: grows slightly) */
    int toast;                     /* S_* or -1 */
    const char *toast_text;        /* free text instead of a string id (item names from events) */
    float toast_a;
    float fade;                    /* 0..1 black */
    int loading;
    /* bonfire menu */
    int menu_on, menu_n, menu_cursor;
    const char *menu_item[4];
    unsigned char menu_enabled[4];
    const char *menu_title;
    int hide_hud;                  /* death / loading: only overlays */
} HudFrame;

typedef struct {
    const UiRes *ui;
    float hp_trail, boss_trail, sp_shown;
    float hp_hold, boss_hold;      /* s before the trail starts to drain */
    float souls_shown;
    float t;
    int inited;
} Hud;

void hud_frame_clear(HudFrame *f);
void hud_init(Hud *h, const UiRes *ui);
void hud_tick(Hud *h, const HudFrame *f, float dt);
void hud_draw(const Hud *h, const HudFrame *f);
/* bloodstain glow at a world point (green, pulsing). vp = projection * view of the frame. */
void hud_draw_stain(const Hud *h, const ScePspFMatrix4 *vp, const float pos[3]);
/* small additive fire glow (bonfire marker for test scenes) */
void hud_draw_fire(const Hud *h, const ScePspFMatrix4 *vp, const float pos[3]);

#endif
