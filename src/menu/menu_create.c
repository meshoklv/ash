/* Character creation (DS1-style): rows Имя / Пол / Класс / Дар / Волосы / Готово, list pop-ups, our own on-screen
 * keyboard (Cyrillic + Latin), class stats and starting gear from menu.ashcls, rotating 3D preview of the class
 * model (tools/cook_classes.py) with the idle clip. See menu.h, docs/menu.md. */
#include <pspkernel.h>
#include <pspgu.h>
#include <pspgum.h>
#include <malloc.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "menu_int.h"
#include "scene.h"

enum { CR_NAME, CR_GENDER, CR_CLASS, CR_GIFT, CR_HAIR, CR_DONE, CR_N };
enum { SUB_ROWS, SUB_LIST, SUB_OSK, SUB_CONFIRM };

#define PREVIEW_CX 262.0f   /* screen x of the character */

static int list_n(const Menu *m, int row)
{
    switch (row) {
    case CR_GENDER: return 2;
    case CR_CLASS: return m->ch->n_class;
    case CR_GIFT: return m->ch->n_gift;
    case CR_HAIR: return 4;
    }
    return 0;
}

static int *row_val(Menu *m, int row)
{
    switch (row) {
    case CR_GENDER: return &m->chr.gender;
    case CR_CLASS: return &m->chr.class_idx;
    case CR_GIFT: return &m->chr.gift_idx;
    case CR_HAIR: return &m->chr.hair;
    }
    return 0;
}

static const MenuHairRec *hair_rec(const Menu *m, int gender, int k)
{
    for (int i = 0; i < m->ch->n_hair; i++)
        if (m->hair[i].gender == gender && m->hair[i].index == k) return &m->hair[i];
    return 0;
}

static void apply_class(Menu *m)
{
    const MenuClassRec *c = &m->cls[m->chr.class_idx];
    m->chr.class_row = c->row;
    m->chr.level = c->level;
    m->chr.humanity = c->humanity;
    memcpy(m->chr.attr, c->attr, MA_N);
}

void create_begin(Menu *m)
{
    if (m->chr.class_idx < 0 || m->chr.class_idx >= m->ch->n_class) m->chr.class_idx = 1;
    apply_class(m);
    m->prev_yaw = 0;
    m->osk_on = 0;
    preview_sync(m);
}

/* ------------------------------------------------------------------ preview models */

static int load_model(Chr *c, u8 **file, int *size, const char *name)
{
    *size = ash_load_file(name, file, 0, 0);
    if (*size <= 0) { printf("ash-menu: preview %s missing\n", name); *file = 0; return -1; }
    int r = chr_load(c, *file, *size);
    if (r != 0) { printf("ash-menu: preview %s bad (%d)\n", name, r); free(*file); *file = 0; return -2; }
    return 0;
}

void preview_sync(Menu *m)
{
    int bk = m->chr.class_idx * 2 + m->chr.gender;
    const MenuClassRec *c = &m->cls[m->chr.class_idx];
    int hk = c->bare_head ? m->chr.gender * 4 + m->chr.hair : -1;
    if (bk != m->body_key) {
        if (m->body_file) { free(m->body.inv_bind); free(m->body_file); m->body_file = 0; }
        char nm[32];
        snprintf(nm, sizeof(nm), "cls_%d_%c.ashc", c->row, m->chr.gender ? 'f' : 'm');
        if (load_model(&m->body, &m->body_file, &m->body_size, nm) == 0) m->load_count++;
        m->body_key = bk;
    }
    if (hk != m->head_key) {
        if (m->head_file) { free(m->head.inv_bind); free(m->head_file); m->head_file = 0; }
        if (hk >= 0) {
            char nm[32];
            snprintf(nm, sizeof(nm), "head_%c_%d.ashc", m->chr.gender ? 'f' : 'm', m->chr.hair);
            if (load_model(&m->head, &m->head_file, &m->head_size, nm) == 0) m->load_count++;
        }
        m->head_key = hk;
    }
}

void preview_draw(Menu *m, float cx, float look_y, float dist)
{
    if (!m->body_file || m->idle_clip < 0) return;
    int nb = m->idle.n_bones;
    if (nb != (int)m->body.hdr->n_bone) return;
    anim_sample(&m->idle, m->idle_clip, m->prev_t, m->pose);
    anim_skin_matrices(nb, m->body.parent, m->pose, m->body.inv_bind, m->world, m->skin);

    sceGuClearDepth(0);
    sceGuClear(GU_DEPTH_BUFFER_BIT);
    sceGuEnable(GU_DEPTH_TEST);
    sceGuDepthMask(GU_FALSE);
    sceGuDisable(GU_BLEND);
    sceGuDisable(GU_FOG);
    sceGuEnable(GU_TEXTURE_2D);
    sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
    sceGuTexFilter(GU_LINEAR, GU_LINEAR);
    sceGuTexWrap(GU_REPEAT, GU_REPEAT);
    sceGuTexScale(1.0f, 1.0f);
    sceGuTexOffset(0.0f, 0.0f);
    sceGuAlphaFunc(GU_GREATER, 0x80, 0xff);
    sceGumMatrixMode(GU_PROJECTION);
    sceGumLoadIdentity();
    ScePspFVector3 sh = {cx / (UI_W * 0.5f) - 1.0f, 0, 0};
    sceGumTranslate(&sh);
    sceGumPerspective(30.0f, (float)UI_W / UI_H, 0.3f, 50.0f);
    sceGumMatrixMode(GU_VIEW);
    sceGumLoadIdentity();
    {
        ScePspFVector3 e = {0, look_y + 0.08f, dist}, a = {0, look_y, 0}, up = {0, 1, 0};
        sceGumLookAt(&e, &a, &up);
    }
    /* key light from the front-left, warm; dim cool ambient [придумал] */
    ScePspFVector3 ld = {-0.5f, 0.6f, 0.65f};
    sceGuLight(0, GU_DIRECTIONAL, GU_DIFFUSE, &ld);
    sceGuLightColor(0, GU_DIFFUSE, 0xffd0e8ff);
    sceGuAmbient(0xff706860);
    sceGuEnable(GU_LIGHT0);
    sceGuEnable(GU_LIGHTING);
    sceGuColor(0xffffffff);
    sceGuAmbientColor(0xffffffff);
    sceGumMatrixMode(GU_MODEL);
    sceGumLoadIdentity();
    sceGumRotateY(3.14159265f + m->prev_yaw + 0.55f * sinf(m->prev_t * 0.6f));
    chr_draw(&m->body, m->skin);
    if (m->head_file && (int)m->head.hdr->n_bone == nb) chr_draw(&m->head, m->skin);
    sceGuDisable(GU_LIGHTING);
    /* restore the game's scene light */
    ScePspFVector3 gd = {m->light_dir[0], m->light_dir[1], m->light_dir[2]};
    sceGuLight(0, GU_DIRECTIONAL, GU_DIFFUSE, &gd);
    sceGuLightColor(0, GU_DIFFUSE, m->light_col);
    sceGuAmbient(m->ambient);
}

/* ------------------------------------------------------------------ on-screen keyboard */

static const char OSK_RU_U[] = "АБВГДЕЁЖЗИЙКЛМНОПРСТУФХЦЧШЩЪЫЬЭЮЯ";
static const char OSK_RU_L[] = "абвгдеёжзийклмнопрстуфхцчшщъыьэюя";
static const char OSK_LA_U[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456";
static const char OSK_LA_L[] = "abcdefghijklmnopqrstuvwxyz0123456";
#define OSK_COLS 11
#define OSK_KEYS 33
enum { OSK_K_LANG = OSK_KEYS, OSK_K_SPACE, OSK_K_DEL, OSK_K_DONE, OSK_TOTAL };

/* k-th UTF-8 code point of s copied to out (zero-terminated) */
static void utf8_at(const char *s, int k, char out[8])
{
    const unsigned char *p = (const unsigned char *)s;
    while (*p && k > 0) {
        p++;
        while ((*p & 0xc0) == 0x80) p++;
        k--;
    }
    int n = 1;
    while ((p[n] & 0xc0) == 0x80) n++;
    memcpy(out, p, n);
    out[n] = 0;
}

static int utf8_len(const char *s)
{
    int n = 0;
    for (; *s; s++) if ((*s & 0xc0) != 0x80) n++;
    return n;
}

static void utf8_pop(char *s)
{
    int n = (int)strlen(s);
    while (n > 0) {
        n--;
        if ((s[n] & 0xc0) != 0x80) break;
    }
    s[n] = 0;
}

static const char *osk_set(const Menu *m)
{
    return m->osk_latin ? (m->osk_lower ? OSK_LA_L : OSK_LA_U) : (m->osk_lower ? OSK_RU_L : OSK_RU_U);
}

static void osk_open(Menu *m)
{
    m->osk_on = 1;
    m->osk_cur = 0;
    m->osk_lower = m->chr.name[0] != 0;
    snprintf(m->osk_buf, sizeof(m->osk_buf), "%s", m->chr.name);
}

static void osk_type(Menu *m, const char *ch)
{
    if (utf8_len(m->osk_buf) >= MENU_NAME_MAX || strlen(m->osk_buf) + strlen(ch) >= sizeof(m->osk_buf)) return;
    strcat(m->osk_buf, ch);
    m->osk_lower = 1;   /* first letter capital, then small [придумал] */
}

/* returns 1 when closed (done or cancelled) */
static int osk_tick(Menu *m)
{
    int c = m->osk_cur;
    int row = c < OSK_KEYS ? c / OSK_COLS : 3, col = c < OSK_KEYS ? c % OSK_COLS : 0;
    if (m->pressed & B_RIGHT) {
        if (row < 3) c = row * OSK_COLS + (col + 1) % OSK_COLS;
        else c = OSK_KEYS + (c - OSK_KEYS + 1) % 4;
    }
    if (m->pressed & B_LEFT) {
        if (row < 3) c = row * OSK_COLS + (col + OSK_COLS - 1) % OSK_COLS;
        else c = OSK_KEYS + (c - OSK_KEYS + 3) % 4;
    }
    if (m->pressed & B_DOWN) {
        if (row < 2) c += OSK_COLS;
        else if (row == 2) c = OSK_KEYS + (col * 4) / OSK_COLS;
        else c = ((c - OSK_KEYS) * OSK_COLS) / 4;
    }
    if (m->pressed & B_UP) {
        if (row > 0 && row < 3) c -= OSK_COLS;
        else if (row == 0) c = OSK_KEYS + (col * 4) / OSK_COLS;
        else c = 2 * OSK_COLS + ((c - OSK_KEYS) * OSK_COLS) / 4 + 1;
    }
    m->osk_cur = c;
    if (m->pressed & B_TRI) m->osk_lower ^= 1;
    if (m->pressed & B_SQ) utf8_pop(m->osk_buf);
    if (m->pressed & B_OK) {
        if (c < OSK_KEYS) { char ch[8]; utf8_at(osk_set(m), c, ch); osk_type(m, ch); }
        else if (c == OSK_K_LANG) m->osk_latin ^= 1;
        else if (c == OSK_K_SPACE) { if (m->osk_buf[0]) osk_type(m, " "); }
        else if (c == OSK_K_DEL) utf8_pop(m->osk_buf);
        else if (c == OSK_K_DONE) {
            snprintf(m->chr.name, sizeof(m->chr.name), "%s", m->osk_buf);
            m->osk_on = 0;
            return 1;
        }
    }
    if (m->pressed & B_START) {
        snprintf(m->chr.name, sizeof(m->chr.name), "%s", m->osk_buf);
        m->osk_on = 0;
        return 1;
    }
    if (m->pressed & B_BACK) { m->osk_on = 0; return 1; }
    return 0;
}

static void osk_draw(Menu *m, float t)
{
    const float x = 56, y = 58, w = 368, h = 178;
    ui_rect(0, 0, UI_W, UI_H, 0x70000000);
    mi_panel(m, x, y, w, h);
    ui_text_sh(&m->ui, F_M, x + w / 2, y + 6, ui_str(&m->ui, MS_ENTER_NAME), MC_GOLD, UI_CENTER);
    /* name field */
    ui_rect(x + 60, y + 28, w - 120, 22, 0xc0000000);
    ui_frame(x + 60, y + 28, w - 120, 22, MC_BORDER);
    int tw = ui_text_w(&m->ui, F_M, m->osk_buf);
    ui_text_sh(&m->ui, F_M, x + w / 2 - tw / 2, y + 30, m->osk_buf, MC_SEL, UI_LEFT);
    if (fmodf(t, 1.0f) < 0.6f) ui_rect(x + w / 2 + tw / 2 + 2, y + 32, 1, 15, MC_GOLD);
    ui_text(&m->ui, F_S, x + w - 64, y + 34, mi_fmt("%d/%d", utf8_len(m->osk_buf), MENU_NAME_MAX), MC_DIM, UI_RIGHT);
    /* keys */
    const char *set = osk_set(m);
    const float gx = x + 19, gy = y + 58, cw = 30, chh = 24;
    for (int k = 0; k < OSK_KEYS; k++) {
        float kx = gx + (k % OSK_COLS) * cw, ky = gy + (k / OSK_COLS) * chh;
        char ch[8];
        utf8_at(set, k, ch);
        if (k == m->osk_cur) mi_cursor(m, kx, ky, cw - 2, chh - 2, t);
        else ui_rect(kx, ky, cw - 2, chh - 2, 0x50000000);
        ui_text_sh(&m->ui, F_M, kx + cw / 2 - 1, ky + 2, ch, k == m->osk_cur ? MC_SEL : MC_TEXT, UI_CENTER);
    }
    const int ctl[4] = {m->osk_latin ? MS_OSK_RUS : MS_OSK_ABC, MS_OSK_SPACE, MS_OSK_DEL, MS_DONE};
    const float bw = (OSK_COLS * cw) / 4;
    for (int i = 0; i < 4; i++) {
        float bx = gx + i * bw, by = gy + 3 * chh + 4;
        int sel = m->osk_cur == OSK_KEYS + i;
        if (sel) mi_cursor(m, bx, by, bw - 4, chh - 2, t);
        else ui_rect(bx, by, bw - 4, chh - 2, 0x50000000);
        ui_text_sh(&m->ui, F_S, bx + bw / 2 - 2, by + 5, ui_str(&m->ui, ctl[i]), sel ? MC_SEL : MC_TEXT, UI_CENTER);
    }
    mi_help(m, ui_str(&m->ui, MS_HELP_OSK));
}

/* ------------------------------------------------------------------ tick */

int create_tick(Menu *m, float dt)
{
    MenuPage *p = top(m);
    /* preview: idle clip + slow turn; L/R turn it by hand */
    m->prev_t += dt;
    /* yaw 0 = facing the camera (+Z); L/R turn by hand, otherwise a slow sway around the front [придумал] */
    if (m->held & B_L) m->prev_yaw -= 2.5f * dt;
    else if (m->held & B_R) m->prev_yaw += 2.5f * dt;
    if (m->osk_on) { osk_tick(m); return MEV_NONE; }
    switch (p->sub) {
    case SUB_ROWS:
        mi_nav(m, &p->cur, CR_N, 1);
        if (m->pressed & B_BACK) { mi_pop(m); return MEV_NONE; }
        if (m->pressed & B_OK) {
            if (p->cur == CR_NAME) { osk_open(m); }
            else if (p->cur == CR_DONE) { p->sub = SUB_CONFIRM; p->sub_cur = 0; }
            else { p->sub = SUB_LIST; p->scroll = *row_val(m, p->cur); p->sub_cur = p->scroll; }
        }
        break;
    case SUB_LIST: {
        int *v = row_val(m, p->cur);
        if (mi_nav(m, &p->sub_cur, list_n(m, p->cur), 1)) {
            *v = p->sub_cur;
            if (p->cur == CR_CLASS) apply_class(m);
            preview_sync(m);
        }
        if (m->pressed & B_OK) p->sub = SUB_ROWS;
        if (m->pressed & B_BACK) {
            *v = p->scroll;
            if (p->cur == CR_CLASS) apply_class(m);
            preview_sync(m);
            p->sub = SUB_ROWS;
        }
        break;
    }
    case SUB_CONFIRM: {
        int r = mi_dialog(m, ui_str(&m->ui, MS_START_ASK), &p->sub_cur);
        if (r == -1) p->sub = SUB_ROWS;
        if (r == 1) {
            if (!m->chr.name[0]) snprintf(m->chr.name, sizeof(m->chr.name), "%s", ui_str(&m->ui, MS_DEFAULT_NAME));
            apply_class(m);
            menu_fill_view_from_char(m, &m->own_view);
            /* the preview models are not needed in the game: give the memory back */
            if (m->body_file) { free(m->body.inv_bind); free(m->body_file); m->body_file = 0; }
            if (m->head_file) { free(m->head.inv_bind); free(m->head_file); m->head_file = 0; }
            m->body_key = m->head_key = -1;
            menu_close(m);
            return MEV_NEW_GAME;
        }
        break;
    }
    }
    return MEV_NONE;
}

/* ------------------------------------------------------------------ draw */

static const char *row_value(Menu *m, int row, int *dim)
{
    *dim = 0;
    const MenuClassRec *c = &m->cls[m->chr.class_idx];
    switch (row) {
    case CR_NAME: if (!m->chr.name[0]) { *dim = 1; return "---"; } return m->chr.name;
    case CR_GENDER: return ui_str(&m->ui, m->chr.gender ? MS_FEMALE : MS_MALE);
    case CR_CLASS: return menu_dstr(m, c->s_name);
    case CR_GIFT: return menu_dstr(m, m->gift[m->chr.gift_idx].s_name);
    case CR_HAIR: {
        const MenuHairRec *h = hair_rec(m, m->chr.gender, m->chr.hair);
        *dim = !c->bare_head;
        return h ? menu_dstr(m, h->s_name) : "";
    }
    }
    return "";
}

static const char *list_item(Menu *m, int row, int i)
{
    switch (row) {
    case CR_GENDER: return ui_str(&m->ui, i ? MS_FEMALE : MS_MALE);
    case CR_CLASS: return menu_dstr(m, m->cls[i].s_name);
    case CR_GIFT: return menu_dstr(m, m->gift[i].s_name);
    case CR_HAIR: { const MenuHairRec *h = hair_rec(m, m->chr.gender, i); return h ? menu_dstr(m, h->s_name) : ""; }
    }
    return "";
}

/* description for the focused row / list entry */
static void draw_desc(Menu *m, float x, float y, float w)
{
    MenuPage *p = top(m);
    int row = p->cur;
    const char *d = "";
    int icon = -1;
    const char *extra = 0;
    const MenuClassRec *c = &m->cls[m->chr.class_idx];
    switch (row) {
    case CR_NAME: d = ui_str(&m->ui, MS_ENTER_NAME); break;
    case CR_GENDER: d = ui_str(&m->ui, m->chr.gender ? MS_FEMALE_DESC : MS_MALE_DESC); break;
    case CR_CLASS: d = menu_dstr(m, c->s_desc); break;
    case CR_GIFT: {
        const MenuGiftRec *g = &m->gift[m->chr.gift_idx];
        d = menu_dstr(m, g->s_desc);
        if (g->item_img != MENU_NONE16) icon = g->item_img;
        if (g->item_str != MENU_NONE16)
            extra = g->count > 1 ? mi_fmt("%s ×%d", menu_dstr(m, g->item_str), g->count) : menu_dstr(m, g->item_str);
        break;
    }
    case CR_HAIR: {
        const MenuHairRec *h = hair_rec(m, m->chr.gender, m->chr.hair);
        d = c->bare_head ? (h ? menu_dstr(m, h->s_desc) : "") : ui_str(&m->ui, MS_HELM_ON);
        break;
    }
    case CR_DONE: d = ui_str(&m->ui, MS_START_ASK); break;
    }
    ui_rect(x, y, w, 1, 0x60607080);
    float tx = x + 2;
    if (icon >= 0) { mi_img(m, icon, x + 1, y + 5, 24, 24, 0xffffffff); tx = x + 28; }
    if (extra && top(m)->sub != SUB_LIST) { ui_text_sh(&m->ui, F_S, tx, y + 4, extra, MC_GOLD, UI_LEFT); y += 14; }
    ui_text_sh(&m->ui, F_S, tx, y + 4, mi_wrap(m, F_S, d, (int)(x + w - tx), 4), MC_TEXT, UI_LEFT);
}

static void draw_stats(Menu *m, float x, float y, float w)
{
    const MenuClassRec *c = &m->cls[m->chr.class_idx];
    mi_panel(m, x, y, w, 196);
    ui_text_sh(&m->ui, F_M, x + w / 2, y + 5, menu_dstr(m, c->s_name), MC_GOLD, UI_CENTER);
    float ly = y + 26;
    ui_text_sh(&m->ui, F_S, x + 10, ly, ui_str(&m->ui, MS_LEVEL), MC_TEXT, UI_LEFT);
    ui_text_sh(&m->ui, F_S, x + w - 12, ly, mi_fmt("%d", c->level), MC_VAL, UI_RIGHT);
    ly += 15;
    ui_rect(x + 8, ly - 2, w - 16, 1, 0x50607080);
    for (int i = 0; i < MA_N; i++) {
        ui_text_sh(&m->ui, F_S, x + 10, ly, ui_str(&m->ui, MS_VIT + i), MC_TEXT, UI_LEFT);
        ui_text_sh(&m->ui, F_S, x + w - 12, ly, mi_fmt("%d", c->attr[i]), MC_VAL, UI_RIGHT);
        ly += 13;
    }
    ui_text_sh(&m->ui, F_S, x + 10, ly, ui_str(&m->ui, MS_WEIGHT), MC_TEXT, UI_LEFT);
    ui_text_sh(&m->ui, F_S, x + w - 12, ly, mi_fmt("%.1f", c->weight), MC_VAL, UI_RIGHT);
    ly += 16;
    /* starting gear icons: weapons, shield, armour, goods */
    int imgs[10], n = 0;
    static const int order[] = {MEQ_R1, MEQ_R2, MEQ_L1, MEQ_HEAD, MEQ_BODY, MEQ_ARMS, MEQ_LEGS};
    for (int i = 0; i < 7; i++) if (c->eq_img[order[i]] != MENU_NONE16) imgs[n++] = c->eq_img[order[i]];
    for (int i = 0; i < c->n_goods && n < 10; i++) if (c->goods_img[i] != MENU_NONE16) imgs[n++] = c->goods_img[i];
    float is = n > 7 ? (w - 12) / n : 19;
    for (int i = 0; i < n; i++) {
        ui_rect(x + 6 + i * is, ly, is - 1, is - 1, 0x60000000);
        mi_img(m, imgs[i], x + 6 + i * is, ly, is - 1, is - 1, 0xffffffff);
    }
}

void create_draw(Menu *m)
{
    MenuPage *p = top(m);
    ui_begin();
    ui_rect(0, 0, UI_W, UI_H, 0xff000000);
    mi_img_tile(m, MI_LEATHER, 0, 0, UI_W, UI_H, 0xff383838);
    ui_vgrad(0, 0, UI_W, UI_H, 0x40000000, 0xc0000000);
    ui_glow(PREVIEW_CX, 150, 110, 0x50183050);      /* warm haze behind the character */
    ui_glow(PREVIEW_CX, 232, 40, 0x30101820);
    mi_header(m, 16, 6, 300, ui_str(&m->ui, MS_CREATE));
    ui_end();
    preview_draw(m, PREVIEW_CX, 0.92f, 4.6f);
    ui_begin();
    /* left: rows or the open list */
    const float lx = 10, ly = 44, lw = 192, lh = 206;
    mi_panel(m, lx, ly, lw, lh);
    if (p->sub == SUB_LIST) {
        int n = list_n(m, p->cur);
        static const int lbl[CR_N] = {MS_NAME, MS_GENDER, MS_CLASS, MS_GIFT, MS_HAIR, MS_DONE};
        ui_text_sh(&m->ui, F_M, lx + 10, ly + 4, ui_str(&m->ui, lbl[p->cur]), MC_GOLD, UI_LEFT);
        float rh = n > 6 ? 12.0f : 18;
        int font = n > 6 ? F_S : F_M;
        for (int i = 0; i < n; i++) {
            float y = ly + 24 + i * rh;
            if (i == p->sub_cur) mi_cursor(m, lx + 4, y - 1, lw - 8, rh, p->t);
            ui_text_sh(&m->ui, font, lx + 14, y + (font == F_S ? 0 : 0), list_item(m, p->cur, i),
                       i == p->sub_cur ? MC_SEL : MC_TEXT, UI_LEFT);
        }
        draw_desc(m, lx + 6, ly + 26 + (n > 6 ? 10 * 12.0f : 4 * 18 + 6), lw - 12);
    } else {
        static const int lbl[CR_N] = {MS_NAME, MS_GENDER, MS_CLASS, MS_GIFT, MS_HAIR, MS_DONE};
        for (int i = 0; i < CR_N; i++) {
            float y = ly + 8 + i * 20;
            if (i == p->cur && p->sub == SUB_ROWS && !m->osk_on) mi_cursor(m, lx + 4, y - 1, lw - 8, 19, p->t);
            u32 col = i == p->cur ? MC_SEL : MC_TEXT;
            if (i == CR_DONE) {
                ui_text_sh(&m->ui, F_M, lx + lw / 2, y, ui_str(&m->ui, MS_DONE), col, UI_CENTER);
                continue;
            }
            ui_text_sh(&m->ui, F_M, lx + 12, y, ui_str(&m->ui, lbl[i]), col, UI_LEFT);
            int dim;
            const char *v = row_value(m, i, &dim);
            ui_text_sh(&m->ui, F_S, lx + lw - 12, y + 3, v, dim ? MC_DIM : MC_VAL, UI_RIGHT);
        }
        draw_desc(m, lx + 6, ly + 8 + CR_N * 20 + 2, lw - 12);
    }
    draw_stats(m, 322, 44, 148);
    mi_help(m, ui_str(&m->ui, p->sub == SUB_ROWS ? MS_HELP_TURN : MS_HELP_SELECT));
    if (m->osk_on) osk_draw(m, p->t);
    if (p->sub == SUB_CONFIRM) mi_dialog_draw(m, ui_str(&m->ui, MS_START_ASK), p->sub_cur, p->t);
    ui_end();
}
