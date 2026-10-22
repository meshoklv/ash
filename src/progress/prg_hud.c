/* Progress -> HudFrame: see prg_hud.h. */
#include "prg_hud.h"

void prg_fill_hud(const Progress *p, const UiRes *ui, HudFrame *f)
{
    const PrgCfg *c = &p->cfg;
    f->hp = *p->hp;
    f->hp_max = *p->hp_max;
    f->estus = p->estus;
    f->estus_max = c->estus_max;
    f->souls = p->souls;
    f->souls_gain = p->souls_gain;
    f->souls_gain_t = p->souls_gain_t;
    f->fade = p->fade;
    f->loading = p->loading;
    f->hide_hud = p->phase == PRG_DYING || p->phase == PRG_LOADING;
    f->banner = p->banner;
    f->banner_a = 0;
    f->banner_scale = 1.0f;
    if (p->banner >= 0) {
        float t = p->banner_t;
        if (t < c->banner_in) f->banner_a = t / c->banner_in;
        else if (p->banner_sticky || t < c->banner_in + c->banner_hold) f->banner_a = 1;
        else f->banner_a = 1 - (t - c->banner_in - c->banner_hold) / c->banner_out;
        if (f->banner_a < 0) f->banner_a = 0;
        /* DS1 "YOU DIED" slowly grows while it is on screen [версия: по видео, на глаз] */
        f->banner_scale = p->banner_sticky ? 0.94f + 0.06f * (t > 4 ? 1 : t / 4) : 1.0f;
    }
    f->toast = p->toast;
    f->toast_a = p->toast >= 0 ? (p->toast_t < 0.3f ? p->toast_t / 0.3f : p->toast_t > 2.4f ? (3.0f - p->toast_t) / 0.6f : 1)
                               : 0;
    f->menu_on = p->phase == PRG_REST && p->sub == 1;
    if (f->menu_on) {
        f->menu_n = p->menu_n;
        f->menu_cursor = p->menu_cursor;
        for (int i = 0; i < p->menu_n && i < 4; i++) {
            f->menu_item[i] = ui_str(ui, p->menu_item[i]);
            f->menu_enabled[i] = p->menu_enabled[i];
        }
        f->menu_title = ui_str(ui, S_AREA_ASYLUM);
    }
}
