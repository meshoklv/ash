/* Ash HUD: see hud.h. Positions are for 480x272 [придумал: по скринам DS1, пропорции HUD 1080p -> 272]. */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "hud.h"

/* colours, ABGR */
#define C_FRAME 0xff2c2c2c
#define C_BG 0xb0101010
#define C_HP_T 0xff2c2cd2
#define C_HP_B 0xff141486
#define C_TRAIL 0xff46b4dc
#define C_SP_T 0xff3cb464
#define C_SP_B 0xff1c6a34
#define C_TEXT 0xffe8e8e8
#define C_GOLD 0xff6ac8e6
#define C_GREY 0xff7a7a7a

#define HP_PX 0.22f       /* bar px per HP point: 572 HP -> 126 px [придумал] */
#define SP_PX 1.35f       /* stamina: 88 -> 119 px [придумал] */

static int imin(int a, int b) { return a < b ? a : b; }
static float clampf(float v, float a, float b) { return v < a ? a : v > b ? b : v; }
static u32 with_a(u32 c, float a) { return ((u32)(clampf(a, 0, 1) * ((c >> 24) & 255)) << 24) | (c & 0x00ffffff); }

void hud_frame_clear(HudFrame *f)
{
    memset(f, 0, sizeof(*f));
    f->banner = -1;
    f->toast = -1;
    f->banner_scale = 1.0f;
    f->estus_max = -1;
}

void hud_init(Hud *h, const UiRes *ui)
{
    memset(h, 0, sizeof(*h));
    h->ui = ui;
}

static void trail(float *tr, float *hold, float v, float vmax, float dt)
{
    if (v >= *tr) { *tr = v; *hold = 0.6f; return; }   /* [версия] DS1 holds the lost part ~0.5 s */
    if (*hold > 0) { *hold -= dt; return; }
    *tr -= vmax * 0.7f * dt;
    if (*tr < v) *tr = v;
}

void hud_tick(Hud *h, const HudFrame *f, float dt)
{
    h->t += dt;
    if (!h->inited) {
        h->hp_trail = f->hp; h->boss_trail = f->boss_hp; h->souls_shown = (float)f->souls; h->sp_shown = f->sp;
        h->inited = 1;
    }
    trail(&h->hp_trail, &h->hp_hold, f->hp, f->hp_max, dt);
    if (f->boss_on) trail(&h->boss_trail, &h->boss_hold, f->boss_hp, f->boss_hp_max, dt);
    else h->boss_trail = f->boss_hp;
    h->sp_shown = f->sp;
    /* souls: the gain waits ~1 s over the counter, then rolls in */
    float target = (f->souls_gain > 0 && f->souls_gain_t < 1.0f) ? (float)(f->souls - f->souls_gain) : (float)f->souls;
    if (target < h->souls_shown) h->souls_shown = target;          /* loss (death) is instant */
    else {
        float d = target - h->souls_shown, step = fmaxf(d * 5.0f * dt, 40.0f * dt);
        h->souls_shown = d <= step ? target : h->souls_shown + step;
    }
}

static void bar(float x, float y, float len, float h, float frac, float trail_frac, u32 top, u32 bot)
{
    ui_rect(x - 1, y - 1, len + 2, h + 2, C_FRAME);
    ui_rect(x, y, len, h, C_BG);
    float tl = clampf(trail_frac, 0, 1) * len, fl = clampf(frac, 0, 1) * len;
    if (tl > fl) ui_rect(x + fl, y, tl - fl, h, C_TRAIL);
    ui_vgrad(x, y, fl, h, top, bot);
    /* thin highlight on top like the DS1 bars */
    if (fl > 1) ui_rect(x, y, fl, 1, 0x40ffffff);
}

static void slot(float cx, float cy, float s, float a)
{
    ui_rect(cx - s / 2, cy - s / 2, s, s, with_a(0xa0101010, a));
    ui_frame(cx - s / 2, cy - s / 2, s, s, with_a(0xff5a5650, a));
}

/* word wrap into buf (max width px) */
static void wrap(const UiRes *ui, int font, const char *s, int maxw, char *buf, int cap)
{
    int o = 0, line_start = 0;
    buf[0] = 0;
    while (*s && o < cap - 2) {
        const char *w = s;
        while (*s && *s != ' ' && *s != '\n') s++;
        int wl = (int)(s - w);
        if (o + wl + 2 >= cap) break;
        char save[256];
        int prev = o;
        if (o > line_start) buf[o++] = ' ';
        memcpy(buf + o, w, wl); o += wl; buf[o] = 0;
        if (ui_text_w(ui, font, buf + line_start) > maxw && prev > line_start) {
            memcpy(save, w, imin(wl, 255)); save[imin(wl, 255)] = 0;
            o = prev;
            buf[o++] = '\n';
            line_start = o;
            memcpy(buf + o, save, wl); o += wl; buf[o] = 0;
        }
        if (*s == '\n') { buf[o++] = '\n'; line_start = o; buf[o] = 0; s++; }
        else if (*s == ' ') s++;
    }
}

static void draw_banner(const Hud *h, const HudFrame *f)
{
    if (f->banner < 0 || f->banner_a <= 0) return;
    const UiRes *ui = h->ui;
    float a = f->banner_a;
    float cy = f->banner == IMG_AREA_ASYLUM ? 96 : 128;
    /* dark band behind, soft edges (DS1) */
    ui_vgrad(0, cy - 34, UI_W, 26, 0x00000000, with_a(0xd0000000, a));
    ui_rect(0, cy - 8, UI_W, 16, with_a(0xd0000000, a));
    ui_vgrad(0, cy + 8, UI_W, 26, with_a(0xd0000000, a), 0x00000000);
    float sc = f->banner_scale * (f->banner == IMG_DIED ? 1.25f : 1.0f);
    if (!ui_img(ui, f->banner, UI_W / 2, cy, sc, with_a(0xffffffff, a))) {
        static const int str_of[IMG_COUNT] = {S_DIED, S_VICTORY, S_BONFIRE_LIT, S_AREA_ASYLUM, S_EMPTY, S_EMPTY};
        u32 col = f->banner == IMG_DIED ? 0xff1c1ca0 : f->banner == IMG_AREA_ASYLUM ? 0xffffffff : 0xff3c90e8;
        int lh = ui_line_h(ui, UI_FONT_L);
        ui_text(ui, UI_FONT_L, UI_W / 2, cy - lh / 2, ui_str(ui, str_of[f->banner]), with_a(col, a), UI_CENTER);
    }
}

static void draw_menu(const Hud *h, const HudFrame *f)
{
    const UiRes *ui = h->ui;
    int lh = ui_line_h(ui, UI_FONT_S);
    float x = 22, y = 54, w = 176, row = lh + 6;
    if (f->menu_title) { float tw = ui_text_w(ui, UI_FONT_S, f->menu_title) + 24; if (tw > w) w = tw; }
    float hh = 30 + row * f->menu_n + 8;
    ui_vgrad(x, y, w, hh, 0xd0141414, 0xd00a0a0a);
    ui_frame(x, y, w, hh, 0xff4a4a4a);
    ui_rect(x + 8, y + 24, w - 16, 1, 0xff3a5a70);
    if (f->menu_title) ui_text_sh(ui, UI_FONT_S, x + 10, y + 6, f->menu_title, C_GOLD, UI_LEFT);
    for (int i = 0; i < f->menu_n; i++) {
        float ry = y + 30 + i * row;
        if (i == f->menu_cursor) {
            ui_hgrad(x + 4, ry - 1, w - 8, row, 0x90306080, 0x10306080);
            ui_rect(x + 4, ry - 1, 2, row, 0xff60b0e0);
        }
        ui_text_sh(ui, UI_FONT_S, x + 14, ry + 2, f->menu_item[i], f->menu_enabled[i] ? C_TEXT : C_GREY, UI_LEFT);
    }
}

static void draw_loading(const Hud *h, const HudFrame *f)
{
    (void)f;
    const UiRes *ui = h->ui;
    /* DS1 loading screens show an item and its description: the Estus Flask */
    ui_img(ui, IMG_ESTUS, 70, 196, 1.5f, 0xffffffff);
    ui_text_sh(ui, UI_FONT_L, 104, 176, ui_str(ui, S_ESTUS), C_TEXT, UI_LEFT);
    char buf[512];
    wrap(ui, UI_FONT_S, ui_str(ui, S_ESTUS_HELP), 330, buf, sizeof(buf));
    ui_text(ui, UI_FONT_S, 104, 200, buf, 0xffb8b8b8, UI_LEFT);
    float blink = 0.55f + 0.45f * sinf(h->t * 4.0f);
    const char *ld = ui_str(ui, S_LOADING);
    char first[96];
    int i = 0;
    while (ld[i] && ld[i] != '\n' && i < 95) { first[i] = ld[i]; i++; }
    first[i] = 0;
    ui_text(ui, UI_FONT_S, UI_W - 16, UI_H - 26, first, with_a(C_TEXT, blink), UI_RIGHT);
}

void hud_draw(const Hud *h, const HudFrame *f)
{
    const UiRes *ui = h->ui;
    char buf[64];
    if (!f->hide_hud && f->fade < 1.0f) {
        /* HP + stamina, top-left */
        float hl = clampf(f->hp_max * HP_PX, 40, 300), sl = clampf(f->sp_max * SP_PX, 40, 260);
        if (f->hp_max > 0) bar(14, 12, hl, 7, f->hp / f->hp_max, h->hp_trail / f->hp_max, C_HP_T, C_HP_B);
        if (f->sp_max > 0) bar(14, 23, sl, 5, h->sp_shown / f->sp_max, 0, C_SP_T, C_SP_B);
        /* quick slots: d-pad cross, Estus in the bottom slot */
        float cx = 44, cy = 212, s = 28, d = 27;
        slot(cx, cy - d, s, 0.6f);
        slot(cx - d, cy, s, 0.6f);
        slot(cx + d, cy, s, 0.6f);
        slot(cx, cy + d, s, 1.0f);
        if (f->estus_max >= 0) {
            ui_img(ui, f->estus > 0 ? IMG_ESTUS : IMG_ESTUS_EMPTY, cx, cy + d, 0.875f, 0xffffffff);
            snprintf(buf, sizeof(buf), "%d", f->estus);
            ui_text_sh(ui, UI_FONT_S, cx + s / 2 + 1, cy + d + s / 2 - ui_line_h(ui, UI_FONT_S) + 4, buf,
                       f->estus > 0 ? C_TEXT : C_GREY, UI_RIGHT);
        }
        /* souls, bottom-right */
        float bw = 86, bh = 18, bx = UI_W - 14 - bw, by = UI_H - 12 - bh;
        ui_vgrad(bx, by, bw, bh, 0xc0181818, 0xc0080808);
        ui_frame(bx, by, bw, bh, 0xff3e3e3e);
        snprintf(buf, sizeof(buf), "%d", (int)(h->souls_shown + 0.5f));
        ui_text_sh(ui, UI_FONT_S, bx + bw - 7, by + 2, buf, C_TEXT, UI_RIGHT);
        if (f->souls_gain > 0 && f->souls_gain_t < 2.0f) {
            float a = f->souls_gain_t < 1.4f ? 1.0f : 1.0f - (f->souls_gain_t - 1.4f) / 0.6f;
            snprintf(buf, sizeof(buf), "+%d", f->souls_gain);
            ui_text_sh(ui, UI_FONT_S, bx + bw - 7, by - 16, buf, with_a(C_TEXT, a), UI_RIGHT);
        }
        /* boss, bottom-centre */
        if (f->boss_on && f->boss_hp_max > 0) {
            float x = 90, w = 300, y = 240;
            if (f->boss_name) ui_text_sh(ui, UI_FONT_S, x, y - 17, f->boss_name, C_TEXT, UI_LEFT);
            bar(x, y, w, 6, f->boss_hp / f->boss_hp_max, h->boss_trail / f->boss_hp_max, C_HP_T, C_HP_B);
        }
        /* action prompt */
        if (f->prompt && f->prompt[0] && !f->menu_on) {
            int tw = ui_text_w(ui, UI_FONT_S, f->prompt), lh = ui_line_h(ui, UI_FONT_S);
            float w = tw + 44, x = UI_W / 2 - w / 2, y = 176;
            ui_vgrad(x, y, w, lh + 8, 0xc8161616, 0xc80a0a0a);
            ui_frame(x, y, w, lh + 8, 0xff5a5a5a);
            ui_rect(x + 8, y + 5, lh - 2, lh - 2, 0xff3a3a3a);
            ui_text(ui, UI_FONT_S, x + 8 + (lh - 2) / 2, y + 3, "×", 0xffe8c078, UI_CENTER);
            ui_text_sh(ui, UI_FONT_S, x + 28, y + 4, f->prompt, C_TEXT, UI_LEFT);
        }
        if (f->menu_on) draw_menu(h, f);
        /* toast */
        const char *tt = f->toast_text ? f->toast_text : f->toast >= 0 ? ui_str(ui, f->toast) : 0;
        if (tt && tt[0] && f->toast_a > 0) {
            int tw = ui_text_w(ui, UI_FONT_S, tt), lh = ui_line_h(ui, UI_FONT_S);
            float w = tw + 30, x = UI_W / 2 - w / 2, y = 44;
            ui_rect(x, y, w, lh + 8, with_a(0xc0101010, f->toast_a));
            ui_frame(x, y, w, lh + 8, with_a(0xff505050, f->toast_a));
            ui_text_sh(ui, UI_FONT_S, UI_W / 2, y + 4, tt, with_a(C_TEXT, f->toast_a), UI_CENTER);
        }
    }
    draw_banner(h, f);
    if (f->fade > 0) ui_rect(0, 0, UI_W, UI_H, with_a(0xff000000, f->fade));
    if (f->loading) draw_loading(h, f);
}

void hud_draw_stain(const Hud *h, const ScePspFMatrix4 *vp, const float pos[3])
{
    float p[3] = {pos[0], pos[1] + 0.15f, pos[2]}, sx, sy, depth;
    if (!ui_project(vp, p, &sx, &sy, &depth)) return;
    float r = clampf(70.0f / depth, 4, 40);
    float pulse = 0.75f + 0.25f * sinf(h->t * 3.0f);
    ui_glow(sx, sy, r * 1.6f, with_a(0xff40d060, 0.45f * pulse));
    ui_glow(sx, sy, r * 0.6f, with_a(0xff90ffb0, 0.8f * pulse));
}

void hud_draw_fire(const Hud *h, const ScePspFMatrix4 *vp, const float pos[3])
{
    float p[3] = {pos[0], pos[1] + 0.5f, pos[2]}, sx, sy, depth;
    if (!ui_project(vp, p, &sx, &sy, &depth)) return;
    float r = clampf(110.0f / depth, 6, 60);
    float fl = 0.8f + 0.12f * sinf(h->t * 11.0f) + 0.08f * sinf(h->t * 17.0f);
    ui_glow(sx, sy, r * 1.5f, with_a(0xff1460e0, 0.5f * fl));
    ui_glow(sx, sy - r * 0.2f, r * 0.6f, with_a(0xff60d0ff, 0.9f * fl));
}
