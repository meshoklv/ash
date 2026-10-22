/* Ash menus: resources, input, screen stack, DS1-style drawing primitives, title / main menu. See menu.h. */
#include <pspkernel.h>
#include <pspgu.h>
#include <pspgum.h>
#include <malloc.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "menu_int.h"
#include "scene.h"   /* ash_load_file */

_Static_assert(sizeof(MenuClsHdr) == 16, "MenuClsHdr");
_Static_assert(sizeof(MenuClassRec) == 72, "MenuClassRec");
_Static_assert(sizeof(MenuGiftRec) == 12, "MenuGiftRec");
_Static_assert(sizeof(MenuHairRec) == 8, "MenuHairRec");

/* ------------------------------------------------------------------ resources */

int menu_init(Menu *m)
{
    memset(m, 0, sizeof(*m));
    m->body_key = m->head_key = -1;
    m->idle_clip = -1;
    char path[256];
    m->ui_size = ash_load_file("menu.ashui", &m->ui_file, path, sizeof(path));
    if (m->ui_size <= 0 || ui_load(&m->ui, m->ui_file, m->ui_size) != 0) {
        printf("ash-menu: menu.ashui missing or bad (%d)\n", m->ui_size);
        return -1;
    }
    m->cls_size = ash_load_file("menu.ashcls", &m->cls_file, 0, 0);
    if (m->cls_size < (int)sizeof(MenuClsHdr) || memcmp(m->cls_file, "ASCL", 4) != 0) {
        printf("ash-menu: menu.ashcls missing or bad (%d)\n", m->cls_size);
        return -2;
    }
    m->ch = (const MenuClsHdr *)m->cls_file;
    m->cls = (const MenuClassRec *)(m->cls_file + sizeof(MenuClsHdr));
    m->gift = (const MenuGiftRec *)(m->cls + m->ch->n_class);
    m->hair = (const MenuHairRec *)(m->gift + m->ch->n_gift);
    m->idle_size = ash_load_file("menu_idle.ashanim", &m->idle_file, 0, 0);
    if (m->idle_size > 0 && anim_load(&m->idle, m->idle_file, m->idle_size) == 0)
        m->idle_clip = anim_find(&m->idle, "idle");
    else
        printf("ash-menu: menu_idle.ashanim missing - preview without animation\n");
    m->light_dir[0] = -0.35f; m->light_dir[1] = 0.8f; m->light_dir[2] = -0.45f;
    m->light_col = 0xff86b2d9; m->ambient = 0xff4a4a4a;   /* = src/hudtest, src/game defaults */
    m->set.scheme = 0; m->set.brightness = 5; m->set.cam_speed = 5;
    m->chr.class_idx = 1; m->chr.gift_idx = 0;              /* DS1 default cursor: Рыцарь [придумал] */
    printf("ash-menu: %s %d B: %u glyphs, %u strings, %u images; classes %u, gifts %u, hair %u\n", path, m->ui_size,
           (unsigned)m->ui.h->n_glyph, (unsigned)m->ui.h->n_str, (unsigned)m->ui.h->n_img, m->ch->n_class,
           m->ch->n_gift, m->ch->n_hair);
    return 0;
}

static void free_model(Chr *c, u8 **file, int *key)
{
    if (*file) { free(c->inv_bind); free(*file); }
    memset(c, 0, sizeof(*c));
    *file = 0;
    *key = -1;
}

void menu_free(Menu *m)
{
    free_model(&m->body, &m->body_file, &m->body_key);
    free_model(&m->head, &m->head_file, &m->head_key);
    free(m->idle_file); free(m->cls_file); free(m->ui_file);
    m->idle_file = m->cls_file = m->ui_file = 0;
}

u32 menu_mem_bytes(const Menu *m)
{
    u32 n = (u32)m->ui_size + (u32)m->cls_size + (u32)(m->idle_size > 0 ? m->idle_size : 0);
    if (m->body_file) n += (u32)m->body_size + m->body.hdr->n_bone * 64;
    if (m->head_file) n += (u32)m->head_size + m->head.hdr->n_bone * 64;
    return n;
}

const char *menu_dstr(const Menu *m, int id)
{
    if (id < 0 || id == MENU_NONE16) return "";
    return ui_str(&m->ui, id);
}

void menu_set_has_save(Menu *m, int has) { m->has_save = has; }
void menu_set_view(Menu *m, MenuGameView *v) { m->view = v; }
void menu_set_scene_light(Menu *m, const float dir[3], u32 diffuse, u32 ambient)
{
    memcpy(m->light_dir, dir, sizeof(m->light_dir));
    m->light_col = diffuse;
    m->ambient = ambient;
}

/* ------------------------------------------------------------------ stack and input */

void mi_push(Menu *m, int screen)
{
    if (m->depth >= MENU_STACK) return;
    MenuPage *p = &m->stack[m->depth++];
    memset(p, 0, sizeof(*p));
    p->screen = screen;
    if (screen == MENU_CREATE) create_begin(m);
    else if (screen >= MENU_PAUSE) game_begin(m, screen);
}

int mi_pop(Menu *m)
{
    if (m->depth > 0) m->depth--;
    return m->depth == 0;
}

void menu_open(Menu *m, int screen)
{
    m->depth = 0;
    m->osk_on = 0;
    if (screen == MENU_NONE) return;
    if (!m->view) { menu_fill_view_from_char(m, &m->own_view); m->view = &m->own_view; }
    mi_push(m, screen);
    if (screen == MENU_MAIN) top(m)->cur = m->has_save ? 0 : 1;
    if (screen == MENU_DIED) m->died_t = 0;
    m->pressed = 0;
    m->prev = 0xffffffffu;   /* a button held while opening does not count as a press */
}

void menu_close(Menu *m) { m->depth = 0; m->osk_on = 0; }
int menu_screen(const Menu *m) { return m->depth ? m->stack[m->depth - 1].screen : MENU_NONE; }

int menu_fullscreen(const Menu *m)
{
    if (!m->depth) return 0;
    int b = m->stack[0].screen;
    return b == MENU_TITLE || b == MENU_MAIN || b == MENU_CREATE;
}

int menu_blocks_game(const Menu *m) { return m->depth && menu_screen(m) != MENU_DIED; }

int mi_nav(Menu *m, int *cur, int n, int wrap)
{
    int d = (m->pressed & B_DOWN) ? 1 : (m->pressed & B_UP) ? -1 : 0;
    if (!d || n <= 0) return 0;
    int c = *cur + d;
    if (wrap) c = (c + n) % n;
    else c = c < 0 ? 0 : c >= n ? n - 1 : c;
    if (c == *cur) return 0;
    *cur = c;
    return 1;
}

int mi_lr(Menu *m) { return (m->pressed & B_RIGHT) ? 1 : (m->pressed & B_LEFT) ? -1 : 0; }

static void input(Menu *m, u32 buttons, float dt)
{
    if (m->prev == 0xffffffffu) m->prev = buttons;
    m->held = buttons;
    m->pressed = buttons & ~m->prev;
    m->prev = buttons;
    /* d-pad auto-repeat: 0.35 s, then every 0.09 s [придумал] */
    u32 dpad = buttons & (B_UP | B_DOWN | B_LEFT | B_RIGHT);
    if (dpad && dpad == m->rep_btn) {
        m->rep_t += dt;
        if (m->rep_t > 0.35f) { m->pressed |= dpad; m->rep_t -= 0.09f; }
    } else {
        m->rep_btn = dpad;
        m->rep_t = 0;
    }
}

/* ------------------------------------------------------------------ title / main */

enum { MAIN_CONTINUE, MAIN_NEW, MAIN_SETTINGS, MAIN_QUIT, MAIN_N };

static int title_tick(Menu *m)
{
    MenuPage *p = top(m);
    if (p->t > 0.8f && m->pressed) {
        m->depth = 0;
        mi_push(m, MENU_MAIN);
        top(m)->cur = m->has_save ? MAIN_CONTINUE : MAIN_NEW;
        top(m)->t = 1.0f;   /* background already faded in */
    }
    return MEV_NONE;
}

static int main_tick(Menu *m)
{
    MenuPage *p = top(m);
    if (p->sub == 1) {
        int r = mi_dialog(m, ui_str(&m->ui, MS_QUIT_ASK), &p->sub_cur);
        if (r == 1) return MEV_QUIT;
        if (r == -1) p->sub = 0;
        return MEV_NONE;
    }
    int c = p->cur;
    if (mi_nav(m, &c, MAIN_N, 1)) {
        if (c == MAIN_CONTINUE && !m->has_save) c = (m->pressed & B_UP) ? MAIN_QUIT : MAIN_NEW;
        p->cur = c;
    }
    if (m->pressed & B_BACK) { m->depth = 0; mi_push(m, MENU_TITLE); top(m)->t = 1.0f; return MEV_NONE; }
    if (m->pressed & B_OK) {
        switch (p->cur) {
        case MAIN_CONTINUE: if (m->has_save) return MEV_CONTINUE; break;
        case MAIN_NEW: mi_push(m, MENU_CREATE); break;
        case MAIN_SETTINGS: mi_push(m, MENU_SETTINGS); break;
        case MAIN_QUIT: p->sub = 1; p->sub_cur = 1; break;
        }
    }
    return MEV_NONE;
}

void mi_bg_title(const Menu *m, float a)
{
    u32 c = (u32)(a * 255.0f) & 0xff;
    ui_rect(0, 0, UI_W, UI_H, 0xff000000);
    mi_img(m, MI_TITLE_BG, 0, 0, UI_W, UI_H, 0xff000000u | (c << 16) | (c << 8) | c);
}

static void title_draw(Menu *m)
{
    MenuPage *p = top(m);
    float a = p->t / 1.5f;
    mi_bg_title(m, a > 1 ? 1 : a);
    if (p->t > 0.8f) {
        float pulse = 0.75f + 0.25f * sinf(p->t * 2.6f);
        u32 al = (u32)(pulse * 255.0f);
        ui_text_sh(&m->ui, F_M, UI_W / 2, 206, ui_str(&m->ui, MS_PRESS_ANY), (al << 24) | (MC_TEXT & 0xffffff), UI_CENTER);
    }
}

static void main_draw(Menu *m)
{
    MenuPage *p = top(m);
    mi_bg_title(m, 1.0f);
    ui_vgrad(0, 168, UI_W, 104, 0x00000000, 0xc0000000);
    static const int ids[MAIN_N] = {MS_CONTINUE, MS_NEW_GAME, MS_SETTINGS, MS_QUIT};
    for (int i = 0; i < MAIN_N; i++) {
        float y = 176 + i * 21;
        const char *s = ui_str(&m->ui, ids[i]);
        int dis = i == MAIN_CONTINUE && !m->has_save;
        if (i == p->cur && p->sub == 0) {
            int w = ui_text_w(&m->ui, F_M, s) + 60;
            mi_cursor(m, UI_W / 2 - w / 2, y - 1, w, 20, p->t);
        }
        ui_text_sh(&m->ui, F_M, UI_W / 2, y, s, dis ? MC_DIM : i == p->cur ? MC_SEL : MC_TEXT, UI_CENTER);
    }
    if (p->sub == 1) mi_dialog_draw(m, ui_str(&m->ui, MS_QUIT_ASK), p->sub_cur, p->t);
}

/* ------------------------------------------------------------------ tick / draw */

int menu_tick(Menu *m, u32 buttons, float dt)
{
    if (!m->depth) { m->prev = buttons; return MEV_NONE; }
    input(m, buttons, dt);
    MenuPage *p = top(m);
    p->t += dt;
    m->dbg_frames++;
    int ev = MEV_NONE;
    switch (p->screen) {
    case MENU_TITLE: ev = title_tick(m); break;
    case MENU_MAIN: ev = main_tick(m); break;
    case MENU_CREATE: ev = create_tick(m, dt); break;
    default: ev = game_tick(m, dt); break;
    }
    return ev;
}

void menu_draw(Menu *m)
{
    if (!m->depth) return;
    int scr = menu_screen(m);
    if (scr == MENU_CREATE) { create_draw(m); return; }   /* has its own 3D pass for the preview */
    ui_begin();
    switch (scr) {
    case MENU_TITLE: title_draw(m); break;
    case MENU_MAIN: main_draw(m); break;
    default:
        if (m->stack[0].screen == MENU_MAIN || m->stack[0].screen == MENU_TITLE) {   /* settings from the title */
            mi_bg_title(m, 0.35f);
        }
        game_draw(m);
        break;
    }
    ui_end();
}

void menu_draw_brightness(const MenuSettings *s)
{
    if (s->brightness == 5) return;
    ui_begin();
    if (s->brightness < 5) {
        ui_rect(0, 0, UI_W, UI_H, ((u32)((5 - s->brightness) * 30) << 24));
    } else {
        u32 v = (u32)((s->brightness - 5) * 9);
        sceGuDisable(GU_TEXTURE_2D);
        sceGuBlendFunc(GU_ADD, GU_FIX, GU_FIX, (v << 16) | (v << 8) | v, 0xffffff);
        ui_rect(0, 0, UI_W, UI_H, 0xffffffff);
        sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
    }
    ui_end();
}

/* ------------------------------------------------------------------ drawing primitives */

typedef struct { float u, v; u32 color; float x, y, z; } FV;
#define FV_TYPE (GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D)

static const UiImgRec *bind_img(const Menu *m, int img, int linear)
{
    const UiRes *r = &m->ui;
    if (img >= MENU_IMG_EXT && m->ext_ui) { r = m->ext_ui; img -= MENU_IMG_EXT; }
    if (!ui_has_img(r, img)) return 0;
    const UiImgRec *im = &r->img[img];
    sceGuClutMode(GU_PSM_8888, 0, 0xff, 0);
    sceGuClutLoad(32, r->file + im->off_clut);
    sceGuTexMode(GU_PSM_T8, 0, 0, 0);
    sceGuTexImage(0, im->tw, im->th, im->tw, r->file + im->off_pix);
    sceGuTexFilter(linear ? GU_LINEAR : GU_NEAREST, linear ? GU_LINEAR : GU_NEAREST);
    sceGuTexFlush();
    sceGuEnable(GU_TEXTURE_2D);
    return im;
}

void menu_set_ext_images(Menu *m, const UiRes *r) { m->ext_ui = r; }

static void quad_uv(float x, float y, float w, float h, float u0, float v0, float u1, float v1, u32 col)
{
    /* GE sprites wider than 64 texels are slow on the PSP: draw in 64-px strips of the destination */
    int strips = (int)ceilf(w / 64.0f);
    if (strips < 1) strips = 1;
    FV *v = sceGuGetMemory(2 * strips * sizeof(FV));
    for (int i = 0; i < strips; i++) {
        float a = (float)i / strips, b = (float)(i + 1) / strips;
        FV *p = &v[2 * i];
        p[0].u = u0 + (u1 - u0) * a; p[0].v = v0; p[0].color = col; p[0].x = x + w * a; p[0].y = y; p[0].z = 0;
        p[1].u = u0 + (u1 - u0) * b; p[1].v = v1; p[1].color = col; p[1].x = x + w * b; p[1].y = y + h; p[1].z = 0;
    }
    sceGuDrawArray(GU_SPRITES, FV_TYPE, 2 * strips, 0, v);
}

void mi_img(const Menu *m, int img, float x, float y, float w, float h, u32 col)
{
    const UiImgRec *im = bind_img(m, img, 1);
    if (!im) return;
    quad_uv(x, y, w, h, 0, 0, im->w, im->h, col);
    ui_begin();   /* src/ui caches its font binding: reset it after binding our own texture */
}

void mi_img_tile(const Menu *m, int img, float x, float y, float w, float h, u32 col)
{
    const UiImgRec *im = bind_img(m, img, 1);
    if (!im) return;
    sceGuTexWrap(GU_REPEAT, GU_REPEAT);
    quad_uv(x, y, w, h, 0, 0, w * im->tw / (float)im->w, h * im->th / (float)im->h, col);
    ui_begin();
}

void mi_panel(const Menu *m, float x, float y, float w, float h)
{
    ui_rect(x, y, w, h, 0xe0000000);
    mi_img_tile(m, MI_LEATHER, x, y, w, h, 0xd0a0a0a0);
    ui_vgrad(x, y, w, h * 0.5f, 0x40000000, 0x00000000);
    ui_frame(x, y, w, h, MC_BORDER);
    ui_frame(x + 2, y + 2, w - 4, h - 4, 0x60204050);
    mi_img(m, MI_GOLD_LINE, x + 4, y - 3, w - 8, 6, 0xffffffff);
    mi_img(m, MI_GOLD_LINE, x + 4, y + h - 3, w - 8, 6, 0xffffffff);
}

void mi_cursor(const Menu *m, float x, float y, float w, float h, float t)
{
    float pulse = 0.75f + 0.25f * sinf(t * 4.0f);
    u32 a = (u32)(pulse * 230.0f);
    mi_img(m, MI_CURSOR, x, y, w, h, (a << 24) | 0x00ffffff);
}

void mi_header(const Menu *m, float x, float y, float w, const char *s)
{
    ui_text_sh(&m->ui, F_L, x, y, s, MC_GOLD, UI_LEFT);
    mi_img(m, MI_GOLD_LINE, x - 4, y + 27, w, 5, 0xffffffff);
}

void mi_help(const Menu *m, const char *s)
{
    ui_hgrad(0, 256, UI_W / 2, 16, 0x00000000, 0xb0000000);
    ui_hgrad(UI_W / 2, 256, UI_W / 2, 16, 0xb0000000, 0x00000000);
    ui_text_sh(&m->ui, F_S, UI_W / 2, 257, s, 0xffb0b8bc, UI_CENTER);
}

void mi_bar(float x, float y, float w, float h, float frac, u32 col)
{
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    ui_rect(x - 1, y - 1, w + 2, h + 2, 0xff101010);
    ui_rect(x, y, w, h, 0xff202428);
    ui_vgrad(x, y, w * frac, h, col, (col & 0xff000000) | ((col & 0xfefefe) >> 1));
}

void mi_slider(float x, float y, float w, int v, int lo, int hi)
{
    ui_rect(x, y + 6, w, 2, 0xff485058);
    for (int i = lo; i <= hi; i++) {
        float px = x + w * (i - lo) / (float)(hi - lo);
        ui_rect(px, y + 4, 1, 6, 0xff384048);
    }
    float px = x + w * (v - lo) / (float)(hi - lo);
    ui_rect(px - 3, y + 1, 7, 12, 0xff101010);
    ui_rect(px - 2, y + 2, 5, 10, MC_GOLD);
}

int mi_dialog(Menu *m, const char *q, int *cur)
{
    (void)q;
    if (m->pressed & (B_LEFT | B_RIGHT | B_UP | B_DOWN)) *cur ^= 1;
    if (m->pressed & B_BACK) return -1;
    if (m->pressed & B_OK) return *cur == 0 ? 1 : -1;
    return 0;
}

void mi_dialog_draw(const Menu *m, const char *q, int cur, float t)
{
    ui_rect(0, 0, UI_W, UI_H, 0x80000000);
    int w = ui_text_w(&m->ui, F_M, q) + 48;
    if (w < 220) w = 220;
    float x = UI_W / 2 - w / 2, y = 96;
    mi_panel(m, x, y, w, 76);
    ui_text_sh(&m->ui, F_M, UI_W / 2, y + 12, q, MC_VAL, UI_CENTER);
    const char *yes = ui_str(&m->ui, MS_YES), *no = ui_str(&m->ui, MS_NO);
    for (int i = 0; i < 2; i++) {
        float bx = UI_W / 2 + (i ? 20 : -100);
        if (cur == i) mi_cursor(m, bx, y + 44, 80, 20, t);
        ui_text_sh(&m->ui, F_M, bx + 40, y + 45, i ? no : yes, cur == i ? MC_SEL : MC_TEXT, UI_CENTER);
    }
}

const char *mi_fmt(const char *fmt, ...)
{
    static char buf[8][96];
    static int k;
    char *b = buf[k++ & 7];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, 96, fmt, ap);
    va_end(ap);
    return b;
}

const char *mi_wrap(const Menu *m, int font, const char *s, int width, int max_lines)
{
    static char out[1024];
    char line[384], word[128];
    int o = 0, lines = 0;
    line[0] = 0;
    out[0] = 0;
    while (*s && lines < max_lines) {
        /* next word (up to space or newline) */
        int n = 0;
        while (s[n] && s[n] != ' ' && s[n] != '\n' && n < (int)sizeof(word) - 1) n++;
        memcpy(word, s, n);
        word[n] = 0;
        char cand[384];
        snprintf(cand, sizeof(cand), "%s%s%s", line, line[0] ? " " : "", word);
        if (line[0] && ui_text_w(&m->ui, font, cand) > width) {
            o += snprintf(out + o, sizeof(out) - o, "%s\n", line);
            if (++lines >= max_lines) break;
            snprintf(line, sizeof(line), "%s", word);
        } else {
            snprintf(line, sizeof(line), "%s", cand);
        }
        s += n;
        if (*s == '\n') {
            o += snprintf(out + o, sizeof(out) - o, "%s\n", line);
            lines++;
            line[0] = 0;
            s++;
        } else if (*s == ' ') {
            s++;
        }
        if (o >= (int)sizeof(out) - 2) break;
    }
    if (lines < max_lines && line[0] && o < (int)sizeof(out) - 1) snprintf(out + o, sizeof(out) - o, "%s", line);
    return out;
}
