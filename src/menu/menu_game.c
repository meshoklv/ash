/* In-game menus (DS1-style): START icon bar, inventory, equipment, status, settings (also from the title), bonfire,
 * level up, "ВЫ ПОГИБЛИ". Data comes from MenuGameView (menu.h) - the game fills it; menu_fill_view_from_char gives
 * the starting state of a freshly created character (stub until src/progress has an inventory). */
#include <pspkernel.h>
#include <pspgu.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "menu_int.h"

enum { PZ_EQUIP, PZ_INV, PZ_STATUS, PZ_SETTINGS, PZ_SYSTEM, PZ_N };
enum { ST_SCHEME, ST_BRIGHT, ST_CAM, ST_INVX, ST_INVY, ST_OVERLAY, ST_DEFAULTS, ST_N };
enum { BF_REST, BF_LEVEL, BF_LEAVE, BF_N };

/* ------------------------------------------------------------------ stub view from the created character */

void menu_fill_view_from_char(const Menu *m, MenuGameView *v)
{
    memset(v, 0, sizeof(*v));
    for (int i = 0; i < MSLOT_COUNT; i++) v->slot[i].img = -1;
    if (!m->ch) return;
    const MenuClassRec *c = &m->cls[m->chr.class_idx < m->ch->n_class ? m->chr.class_idx : 0];
    v->name = m->chr.name[0] ? m->chr.name : ui_str(&m->ui, MS_DEFAULT_NAME);
    v->class_name = menu_dstr(m, c->s_name);
    v->level = c->level;
    v->humanity = c->humanity;
    memcpy(v->attr, c->attr, MA_N);
    /* HP / stamina / load: the real numbers belong to src/combat + src/progress; these are placeholders
       [версия: таблицы DS1 по памяти - HP ≈ 400 + 20*(VIT-8)... не из данных] */
    v->hp_max = v->hp = 400 + 20.0f * (c->attr[MA_VIT] - 8);
    v->sp_max = v->sp = 80 + 2.0f * (c->attr[MA_END] - 8);
    v->load_max = 40 + c->attr[MA_END];
    v->load = c->weight;
    static const int map[MEQ_N] = {MSLOT_R1, MSLOT_R2, MSLOT_L1, MSLOT_L2, MSLOT_HEAD, MSLOT_BODY, MSLOT_ARMS, MSLOT_LEGS};
    for (int i = 0; i < MEQ_N; i++) {
        if (c->eq_str[i] == MENU_NONE16) continue;
        MenuItem *s = &v->slot[map[i]];
        s->name = menu_dstr(m, c->eq_str[i]);
        s->img = c->eq_img[i] == MENU_NONE16 ? -1 : c->eq_img[i];
    }
    /* inventory: Estus (5), class goods, gift */
    MenuItem *it = &v->item[v->n_items++];
    it->name = ui_str(&m->ui, MS_ESTUS); it->desc = ui_str(&m->ui, MS_ESTUS_DESC); it->count = 5; it->img = MI_ICON_ESTUS;
    v->slot[MSLOT_Q1] = *it;
    for (int i = 0; i < c->n_goods && v->n_items < MENU_MAX_ITEMS; i++) {
        it = &v->item[v->n_items++];
        it->name = menu_dstr(m, c->goods_str[i]);
        it->desc = menu_dstr(m, c->goods_desc[i]);
        it->count = c->goods_cnt[i];
        it->img = c->goods_img[i] == MENU_NONE16 ? -1 : c->goods_img[i];
    }
    const MenuGiftRec *g = &m->gift[m->chr.gift_idx < m->ch->n_gift ? m->chr.gift_idx : 0];
    if (g->item_str != MENU_NONE16 && v->n_items < MENU_MAX_ITEMS) {
        it = &v->item[v->n_items++];
        it->name = menu_dstr(m, g->item_str);
        it->desc = menu_dstr(m, g->item_desc);
        it->count = g->count;
        it->img = g->item_img == MENU_NONE16 ? -1 : g->item_img;
    }
    /* weapons and armour also show up in the inventory list */
    for (int i = 0; i < MEQ_N && v->n_items < MENU_MAX_ITEMS; i++) {
        if (c->eq_str[i] == MENU_NONE16) continue;
        it = &v->item[v->n_items++];
        it->name = menu_dstr(m, c->eq_str[i]);
        it->img = c->eq_img[i] == MENU_NONE16 ? -1 : c->eq_img[i];
    }
}

/* ------------------------------------------------------------------ level up cost */

/* Souls to go from level L to L+1. DS1: table for 1..11, then 0.02x^3 + 3.06x^2 + 105.6x - 895 with x = L+1
   [версия: формула и таблица по памяти (вики DS1), в PARAM DSR их нет - сверить в игре] */
static int level_cost(int L)
{
    static const int t[12] = {0, 673, 690, 707, 724, 741, 758, 775, 793, 811, 829, 847};
    if (L >= 1 && L <= 11) return t[L];
    float x = (float)(L + 1);
    return (int)(0.02f * x * x * x + 3.06f * x * x + 105.6f * x - 895.0f + 0.5f);
}

/* ------------------------------------------------------------------ begin / tick */

void game_begin(Menu *m, int screen)
{
    (void)screen;
    if (!m->view) { menu_fill_view_from_char(m, &m->own_view); m->view = &m->own_view; }
    if (screen == MENU_LEVELUP) {
        memcpy(m->lv_attr, m->view->attr, MA_N);
        m->lv_level = m->view->level;
        m->lv_souls = m->view->souls;
    }
    if (screen == MENU_DIED) m->died_t = 0;
}

static int back(Menu *m)
{
    if (mi_pop(m)) return MEV_CLOSE;
    return MEV_NONE;
}

static int settings_change(Menu *m, int row, int d)
{
    MenuSettings *s = &m->set;
    switch (row) {
    case ST_SCHEME: s->scheme ^= 1; break;
    case ST_BRIGHT: s->brightness += d; if (s->brightness < 0) s->brightness = 0; if (s->brightness > 10) s->brightness = 10; break;
    case ST_CAM: s->cam_speed += d; if (s->cam_speed < 1) s->cam_speed = 1; if (s->cam_speed > 10) s->cam_speed = 10; break;
    case ST_INVX: s->invert_x ^= 1; break;
    case ST_INVY: s->invert_y ^= 1; break;
    case ST_OVERLAY: s->overlay ^= 1; break;
    default: return MEV_NONE;
    }
    return MEV_SETTINGS;
}

int game_tick(Menu *m, float dt)
{
    MenuPage *p = top(m);
    MenuGameView *v = m->view;
    switch (p->screen) {
    case MENU_PAUSE:
        if (p->sub == 1) {   /* Система: "В главное меню?" */
            int r = mi_dialog(m, 0, &p->sub_cur);
            if (r == 1) { menu_close(m); return MEV_TO_TITLE; }
            if (r == -1) p->sub = 0;
            return MEV_NONE;
        }
        if (m->pressed & B_RIGHT) p->cur = (p->cur + 1) % PZ_N;
        if (m->pressed & B_LEFT) p->cur = (p->cur + PZ_N - 1) % PZ_N;
        if (m->pressed & (B_BACK | B_START)) return back(m);
        if (m->pressed & B_OK) {
            static const int to[PZ_N] = {MENU_EQUIP, MENU_INVENTORY, MENU_STATUS, MENU_SETTINGS, 0};
            if (p->cur == PZ_SYSTEM) { p->sub = 1; p->sub_cur = 1; }
            else mi_push(m, to[p->cur]);
        }
        return MEV_NONE;
    case MENU_INVENTORY:
        if (mi_nav(m, &p->cur, v->n_items, 0)) {
            if (p->cur < p->scroll) p->scroll = p->cur;
            if (p->cur >= p->scroll + 7) p->scroll = p->cur - 6;
        }
        if (m->pressed & B_BACK) return back(m);
        if ((m->pressed & B_OK) && p->cur < v->n_items && v->item[p->cur].count > 0) {
            m->ev_arg = p->cur;
            return MEV_USE_ITEM;
        }
        return MEV_NONE;
    case MENU_EQUIP: {
        /* grid rows: weapons R1 R2 L1 L2 | arrows/bolts | armour | rings + quick items */
        static const int rs[4] = {0, 4, 8, 12}, rn[4] = {4, 4, 4, 7};
        int row = p->cur < 4 ? 0 : p->cur < 8 ? 1 : p->cur < 12 ? 2 : 3, col = p->cur - rs[row];
        if (m->pressed & B_DOWN) { row = (row + 1) % 4; }
        if (m->pressed & B_UP) { row = (row + 3) % 4; }
        int d = mi_lr(m);
        col = (col + d + rn[row]) % rn[row];
        if (col >= rn[row]) col = rn[row] - 1;
        p->cur = rs[row] + col;
        if (m->pressed & B_BACK) return back(m);
        return MEV_NONE;
    }
    case MENU_STATUS:
        if (m->pressed & (B_BACK | B_OK)) return back(m);
        return MEV_NONE;
    case MENU_SETTINGS: {
        mi_nav(m, &p->cur, ST_N, 1);
        int d = mi_lr(m), ev = MEV_NONE;
        if (d) ev = settings_change(m, p->cur, d);
        if ((m->pressed & B_OK) && p->cur == ST_DEFAULTS) {
            m->set.scheme = 0; m->set.brightness = 5; m->set.cam_speed = 5;
            m->set.invert_x = m->set.invert_y = 0; m->set.overlay = 0;
            ev = MEV_SETTINGS;
        } else if ((m->pressed & B_OK) && p->cur != ST_BRIGHT && p->cur != ST_CAM) {
            ev = settings_change(m, p->cur, 1);
        }
        if (m->pressed & B_BACK) { int e = back(m); return e != MEV_NONE ? e : ev; }
        return ev;
    }
    case MENU_BONFIRE:
        mi_nav(m, &p->cur, BF_N, 1);
        if (m->pressed & B_OK) {
            if (p->cur == BF_REST) return MEV_REST;
            if (p->cur == BF_LEVEL) { mi_push(m, MENU_LEVELUP); return MEV_NONE; }
            menu_close(m);
            return MEV_LEAVE_BONFIRE;
        }
        if (m->pressed & B_BACK) { menu_close(m); return MEV_LEAVE_BONFIRE; }
        return MEV_NONE;
    case MENU_LEVELUP: {
        if (p->sub == 1) {
            int r = mi_dialog(m, 0, &p->sub_cur);
            if (r == 1) {
                memcpy(v->attr, m->lv_attr, MA_N);
                v->level = m->lv_level;
                v->souls = m->lv_souls;
                mi_pop(m);
                return MEV_LEVELUP;
            }
            if (r == -1) p->sub = 0;
            return MEV_NONE;
        }
        mi_nav(m, &p->cur, MA_N + 1, 1);   /* + "Готово" */
        int d = mi_lr(m);
        if (p->cur < MA_N && d > 0 && m->lv_attr[p->cur] < 99) {
            int cost = level_cost(m->lv_level);
            if (m->lv_souls >= cost) { m->lv_souls -= cost; m->lv_level++; m->lv_attr[p->cur]++; }
        } else if (p->cur < MA_N && d < 0 && m->lv_attr[p->cur] > v->attr[p->cur]) {
            m->lv_level--; m->lv_souls += level_cost(m->lv_level); m->lv_attr[p->cur]--;
        }
        if ((m->pressed & B_OK) && p->cur == MA_N && m->lv_level != v->level) { p->sub = 1; p->sub_cur = 0; }
        if (m->pressed & B_BACK) { mi_pop(m); return MEV_NONE; }
        return MEV_NONE;
    }
    case MENU_DIED:
        m->died_t += dt;
        if (m->died_t >= 5.2f) { menu_close(m); return MEV_DIED_DONE; }
        return MEV_NONE;
    }
    return MEV_NONE;
}

/* ------------------------------------------------------------------ draw helpers */

static void kv(const Menu *m, float x, float y, float w, const char *k, const char *val, u32 vc)
{
    ui_text_sh(&m->ui, F_S, x, y, k, MC_TEXT, UI_LEFT);
    ui_text_sh(&m->ui, F_S, x + w, y, val, vc, UI_RIGHT);
}

static void item_icon(const Menu *m, const MenuItem *it, float x, float y, float s, int fallback)
{
    ui_rect(x, y, s, s, 0x80000000);
    ui_frame(x, y, s, s, 0x80405060);
    if (it && it->img >= 0) mi_img(m, it->img, x + 1, y + 1, s - 2, s - 2, 0xffffffff);
    else if (fallback >= 0) mi_img(m, fallback, x + 3, y + 3, s - 6, s - 6, 0x70ffffff);
}

/* ------------------------------------------------------------------ screens */

static void pause_draw(Menu *m, MenuPage *p)
{
    static const int icon[PZ_N] = {MI_ICON_EQUIP, MI_ICON_INV, MI_ICON_STATUS, MI_ICON_SYSTEM, MI_ICON_MAGIC};
    static const int name[PZ_N] = {MS_EQUIPMENT, MS_INVENTORY, MS_STATUS, MS_SETTINGS, MS_SYSTEM};
    const float x = 12, y = 12, s = 40, gap = 6, w = PZ_N * (s + gap) + gap + 8;
    mi_panel(m, x, y, w, s + 34);
    for (int i = 0; i < PZ_N; i++) {
        float ix = x + 4 + gap + i * (s + gap);
        int sel = i == p->cur && m->depth == 1;
        if (i == p->cur) { ui_rect(ix - 3, y + 5, s + 6, s + 6, 0xa04080a0); ui_frame(ix - 3, y + 5, s + 6, s + 6, MC_GOLD); }
        mi_img(m, icon[i], ix, y + 8, s, s, i == p->cur ? 0xffffffff : 0xff909090);
        (void)sel;
    }
    ui_text_sh(&m->ui, F_M, x + w / 2, y + s + 12, ui_str(&m->ui, name[p->cur]), MC_SEL, UI_CENTER);
    if (m->depth == 1) mi_help(m, ui_str(&m->ui, MS_HELP_SELECT));
    if (p->sub == 1) {
        mi_dialog_draw(m, ui_str(&m->ui, MS_TO_TITLE_ASK), p->sub_cur, p->t);
    }
}

static void inventory_draw(Menu *m, MenuPage *p)
{
    const MenuGameView *v = m->view;
    const float x = 10, y = 30, w = 250, h = 218;
    mi_header(m, x + 6, 2, w, ui_str(&m->ui, MS_INVENTORY));
    mi_panel(m, x, y + 6, w, h);
    for (int r = 0; r < 7 && p->scroll + r < v->n_items; r++) {
        int i = p->scroll + r;
        const MenuItem *it = &v->item[i];
        float ry = y + 14 + r * 29;
        if (i == p->cur) mi_cursor(m, x + 4, ry - 2, w - 8, 28, p->t);
        item_icon(m, it, x + 10, ry, 24, MI_SLOT_ITEM);
        ui_text_sh(&m->ui, F_M, x + 42, ry + 3, it->name ? it->name : "", i == p->cur ? MC_SEL : MC_TEXT, UI_LEFT);
        if (it->count > 0) ui_text_sh(&m->ui, F_S, x + w - 14, ry + 6, mi_fmt("%d", it->count), MC_VAL, UI_RIGHT);
    }
    if (v->n_items > 7) {   /* scroll bar */
        float sh = h * 7 / (float)v->n_items;
        ui_rect(x + w - 4, y + 10, 2, h - 8, 0x60303840);
        ui_rect(x + w - 4, y + 10 + (h - 8 - sh) * p->scroll / (float)(v->n_items - 7), 2, sh, MC_GOLD);
    }
    /* details */
    const float dx = 268, dw = 202;
    mi_panel(m, dx, y + 6, dw, h);
    if (p->cur < v->n_items) {
        const MenuItem *it = &v->item[p->cur];
        item_icon(m, it, dx + dw / 2 - 30, y + 16, 60, MI_SLOT_ITEM);
        ui_text_sh(&m->ui, F_M, dx + dw / 2, y + 80, it->name ? it->name : "", MC_GOLD, UI_CENTER);
        if (it->desc) ui_text_sh(&m->ui, F_S, dx + 8, y + 100, mi_wrap(m, F_S, it->desc, (int)dw - 16, 8), MC_TEXT, UI_LEFT);
    }
    mi_help(m, mi_fmt("× %s   ○ %s", ui_str(&m->ui, MS_USE), "Назад"));
}

static const int slot_icon[MSLOT_COUNT] = {
    MI_SLOT_WEAPON, MI_SLOT_WEAPON, MI_SLOT_SHIELD, MI_SLOT_SHIELD, MI_SLOT_ARROW, MI_SLOT_ARROW, MI_SLOT_BOLT,
    MI_SLOT_BOLT, MI_SLOT_HELM, MI_SLOT_ARMOR, MI_SLOT_ARMOR, MI_SLOT_ARMOR, MI_SLOT_RING, MI_SLOT_RING,
    MI_SLOT_ITEM, MI_SLOT_ITEM, MI_SLOT_ITEM, MI_SLOT_ITEM, MI_SLOT_ITEM};

static int slot_label(int s)
{
    if (s <= MSLOT_R2) return MS_RIGHT;
    if (s <= MSLOT_L2) return MS_LEFT;
    if (s <= MSLOT_ARROW2) return MS_ARROW;
    if (s <= MSLOT_BOLT2) return MS_BOLT;
    if (s == MSLOT_HEAD) return MS_HEAD;
    if (s == MSLOT_BODY) return MS_BODY;
    if (s == MSLOT_ARMS) return MS_ARMS;
    if (s == MSLOT_LEGS) return MS_LEGS;
    if (s <= MSLOT_RING2) return MS_RING;
    return MS_QUICK;
}

static void equip_draw(Menu *m, MenuPage *p)
{
    const MenuGameView *v = m->view;
    const float x = 8, y = 34, w = 304, h = 214;
    mi_header(m, x + 8, 2, w, ui_str(&m->ui, MS_EQUIPMENT));
    mi_panel(m, x, y, w, h);
    static const int rs[4] = {0, 4, 8, 12}, rn[4] = {4, 4, 4, 7};
    static const int grp[4][2] = {{MS_RIGHT, MS_LEFT}, {MS_ARROW, MS_BOLT}, {-1, -1}, {MS_RING, MS_QUICK}};
    const float bs = 30, gap = 4, ggap = 18;   /* ggap: between the two groups of a row */
    for (int r = 0; r < 4; r++) {
        float ry = y + 10 + r * 46;
        if (grp[r][0] >= 0) {
            ui_text_sh(&m->ui, F_S, x + 12, ry, ui_str(&m->ui, grp[r][0]), MC_GOLD, UI_LEFT);
            ui_text_sh(&m->ui, F_S, x + 12 + 2 * (bs + gap) + ggap, ry, ui_str(&m->ui, grp[r][1]), MC_GOLD, UI_LEFT);
        } else {
            ui_text_sh(&m->ui, F_S, x + 12, ry, ui_str(&m->ui, MS_HEAD), MC_GOLD, UI_LEFT);
        }
        for (int c = 0; c < rn[r]; c++) {
            int s = rs[r] + c;
            float bx = x + 12 + c * (bs + gap) + (c >= 2 && r != 2 ? ggap : 0), by = ry + 14;
            if (s == p->cur) { ui_rect(bx - 3, by - 3, bs + 6, bs + 6, 0x904080a0); ui_frame(bx - 3, by - 3, bs + 6, bs + 6, MC_GOLD); }
            const MenuItem *it = &v->slot[s];
            ui_rect(bx, by, bs, bs, 0xa0000000);
            ui_frame(bx, by, bs, bs, 0xa0405868);
            if (it->name && it->img >= 0) mi_img(m, it->img, bx + 1, by + 1, bs - 2, bs - 2, 0xffffffff);
            else mi_img(m, slot_icon[s], bx + 4, by + 4, bs - 8, bs - 8, 0x60ffffff);
            if (it->count > 0) ui_text_sh(&m->ui, F_S, bx + bs - 1, by + bs - 13, mi_fmt("%d", it->count), MC_VAL, UI_RIGHT);
        }
    }
    /* focused slot: label + item */
    const MenuItem *it = &v->slot[p->cur];
    ui_rect(x + 8, y + h - 24, w - 16, 1, 0x60607080);
    ui_text_sh(&m->ui, F_S, x + 12, y + h - 19, ui_str(&m->ui, slot_label(p->cur)), MC_GOLD, UI_LEFT);
    ui_text_sh(&m->ui, F_M, x + w - 12, y + h - 21, it->name ? it->name : ui_str(&m->ui, MS_EMPTY_SLOT),
               it->name ? MC_SEL : MC_DIM, UI_RIGHT);
    /* stats */
    const float sx = 318, sw = 154;
    mi_panel(m, sx, y, sw, h);
    float ly = y + 10;
    ui_text_sh(&m->ui, F_M, sx + sw / 2, ly - 4, v->name ? v->name : "", MC_GOLD, UI_CENTER);
    ly += 20;
    kv(m, sx + 8, ly, sw - 16, ui_str(&m->ui, MS_HP), mi_fmt("%.0f/%.0f", v->hp, v->hp_max), MC_VAL); ly += 14;
    kv(m, sx + 8, ly, sw - 16, ui_str(&m->ui, MS_STAMINA), mi_fmt("%.0f/%.0f", v->sp, v->sp_max), MC_VAL); ly += 14;
    kv(m, sx + 8, ly, sw - 16, ui_str(&m->ui, MS_LOAD), mi_fmt("%.0f/%.0f", v->load, v->load_max),
       v->load > v->load_max * 0.5f ? 0xff60a0e0 : MC_VAL); ly += 14;
    mi_bar(sx + 10, ly + 2, sw - 20, 4, v->load_max > 0 ? v->load / v->load_max : 0, 0xff60a0c0); ly += 12;
    kv(m, sx + 8, ly, sw - 16, ui_str(&m->ui, MS_POISE), mi_fmt("%.0f", v->poise), MC_VAL); ly += 14;
    kv(m, sx + 8, ly, sw - 16, ui_str(&m->ui, MS_PHYS_DEF), mi_fmt("%.0f", v->phys_def), MC_VAL); ly += 18;
    ui_rect(sx + 8, ly - 4, sw - 16, 1, 0x60607080);
    kv(m, sx + 8, ly, sw - 16, ui_str(&m->ui, MS_ATK_R1), mi_fmt("%d", v->atk_r1), MC_VAL); ly += 14;
    kv(m, sx + 8, ly, sw - 16, ui_str(&m->ui, MS_ATK_L1), mi_fmt("%d", v->atk_l1), MC_VAL); ly += 14;
    mi_help(m, "◀▶▲▼ Слот   ○ Назад");
}

static void status_draw(Menu *m, MenuPage *p)
{
    (void)p;
    const MenuGameView *v = m->view;
    const float x = 10, y = 34, w = 460, h = 214;
    mi_header(m, x + 8, 2, 300, ui_str(&m->ui, MS_STATUS));
    mi_panel(m, x, y, w, h);
    const float lx = x + 12, lw = 200;
    float ly = y + 10;
    ui_text_sh(&m->ui, F_M, lx, ly - 2, v->name ? v->name : "", MC_GOLD, UI_LEFT);
    ui_text_sh(&m->ui, F_S, lx + lw, ly + 1, v->class_name ? v->class_name : "", MC_TEXT, UI_RIGHT);
    ly += 20;
    kv(m, lx, ly, lw, ui_str(&m->ui, MS_LEVEL), mi_fmt("%d", v->level), MC_VAL); ly += 14;
    kv(m, lx, ly, lw, ui_str(&m->ui, MS_SOULS), mi_fmt("%d", v->souls), MC_VAL); ly += 14;
    kv(m, lx, ly, lw, ui_str(&m->ui, MS_HUMANITY), mi_fmt("%d", v->humanity), MC_VAL); ly += 18;
    ui_rect(lx, ly - 4, lw, 1, 0x60607080);
    for (int i = 0; i < MA_N; i++) {
        kv(m, lx, ly, lw, ui_str(&m->ui, MS_VIT + i), mi_fmt("%d", v->attr[i]), MC_VAL);
        ly += 14;
    }
    ui_rect(x + 230, y + 12, 1, h - 24, 0x60607080);
    const float rx = x + 246, rw = 200;
    ly = y + 30;
    kv(m, rx, ly, rw, ui_str(&m->ui, MS_HP), mi_fmt("%.0f/%.0f", v->hp, v->hp_max), MC_VAL); ly += 14;
    mi_bar(rx, ly + 2, rw, 4, v->hp_max > 0 ? v->hp / v->hp_max : 0, 0xff2828b0); ly += 12;
    kv(m, rx, ly, rw, ui_str(&m->ui, MS_STAMINA), mi_fmt("%.0f/%.0f", v->sp, v->sp_max), MC_VAL); ly += 14;
    mi_bar(rx, ly + 2, rw, 4, v->sp_max > 0 ? v->sp / v->sp_max : 0, 0xff30a040); ly += 12;
    kv(m, rx, ly, rw, ui_str(&m->ui, MS_LOAD), mi_fmt("%.1f/%.1f", v->load, v->load_max), MC_VAL); ly += 18;
    kv(m, rx, ly, rw, ui_str(&m->ui, MS_POISE), mi_fmt("%.0f", v->poise), MC_VAL); ly += 14;
    kv(m, rx, ly, rw, ui_str(&m->ui, MS_PHYS_DEF), mi_fmt("%.0f", v->phys_def), MC_VAL); ly += 14;
    kv(m, rx, ly, rw, ui_str(&m->ui, MS_ATK_R1), mi_fmt("%d", v->atk_r1), MC_VAL); ly += 14;
    kv(m, rx, ly, rw, ui_str(&m->ui, MS_ATK_L1), mi_fmt("%d", v->atk_l1), MC_VAL); ly += 18;
    kv(m, rx, ly, rw, ui_str(&m->ui, MS_COVENANT), ui_str(&m->ui, MS_NONE), MC_VAL);
    mi_help(m, "○ Назад");
}

static void settings_draw(Menu *m, MenuPage *p)
{
    const MenuSettings *s = &m->set;
    const float x = 60, y = 40, w = 360, h = 206;
    mi_header(m, x + 4, 6, 280, ui_str(&m->ui, MS_SETTINGS));
    mi_panel(m, x, y, w, h);
    static const int lbl[ST_N] = {MS_SCHEME, MS_BRIGHTNESS, MS_CAM_SPEED, MS_CAM_X, MS_CAM_Y, MS_OVERLAY, MS_DEFAULTS};
    for (int i = 0; i < ST_N; i++) {
        float ry = y + 10 + i * 21;
        if (i == p->cur) mi_cursor(m, x + 4, ry - 1, w - 8, 20, p->t);
        u32 col = i == p->cur ? MC_SEL : MC_TEXT;
        if (i == ST_DEFAULTS) { ui_text_sh(&m->ui, F_M, x + w / 2, ry, ui_str(&m->ui, lbl[i]), col, UI_CENTER); continue; }
        ui_text_sh(&m->ui, F_M, x + 14, ry, ui_str(&m->ui, lbl[i]), col, UI_LEFT);
        const float vx = x + 200, vw = 140;
        switch (i) {
        case ST_SCHEME: ui_text_sh(&m->ui, F_M, vx + vw / 2, ry, s->scheme ? "B" : "A", MC_VAL, UI_CENTER); break;
        case ST_BRIGHT: mi_slider(vx, ry + 2, vw, s->brightness, 0, 10); break;
        case ST_CAM: mi_slider(vx, ry + 2, vw, s->cam_speed, 1, 10); break;
        case ST_INVX: ui_text_sh(&m->ui, F_S, vx + vw / 2, ry + 2, ui_str(&m->ui, s->invert_x ? MS_INVERT : MS_NORMAL), MC_VAL, UI_CENTER); break;
        case ST_INVY: ui_text_sh(&m->ui, F_S, vx + vw / 2, ry + 2, ui_str(&m->ui, s->invert_y ? MS_INVERT : MS_NORMAL), MC_VAL, UI_CENTER); break;
        case ST_OVERLAY: ui_text_sh(&m->ui, F_S, vx + vw / 2, ry + 2, ui_str(&m->ui, s->overlay ? MS_ON : MS_OFF), MC_VAL, UI_CENTER); break;
        }
        if (i == p->cur && i < ST_DEFAULTS) {
            ui_text(&m->ui, F_S, vx - 8, ry + 2, "◀", MC_GOLD, UI_CENTER);
            ui_text(&m->ui, F_S, vx + vw + 8, ry + 2, "▶", MC_GOLD, UI_CENTER);
        }
    }
    /* hint under the rows */
    float hy = y + 10 + ST_N * 21 + 4;
    ui_rect(x + 10, hy - 2, w - 20, 1, 0x60607080);
    if (p->cur == ST_BRIGHT) {
        ui_text_sh(&m->ui, F_S, x + w / 2, hy + 2, ui_str(&m->ui, MS_BRIGHT_HINT1), MC_TEXT, UI_CENTER);
        ui_text_sh(&m->ui, F_S, x + w / 2, hy + 16, ui_str(&m->ui, MS_BRIGHT_HINT2), MC_TEXT, UI_CENTER);
    } else if (p->cur == ST_SCHEME) {
        ui_text_sh(&m->ui, F_S, x + w / 2, hy + 2, ui_str(&m->ui, s->scheme ? MS_SCHEME_B : MS_SCHEME_A), MC_TEXT, UI_CENTER);
    }
    mi_help(m, ui_str(&m->ui, MS_HELP_CHANGE));
}

static void bonfire_draw(Menu *m, MenuPage *p)
{
    const float x = 24, y = 84, w = 200, h = 82;
    mi_panel(m, x, y, w, h);
    static const int lbl[BF_N] = {MS_REST, MS_LEVELUP, MS_LEAVE};
    for (int i = 0; i < BF_N; i++) {
        float ry = y + 10 + i * 22;
        if (i == p->cur && m->depth == 1) mi_cursor(m, x + 4, ry - 1, w - 8, 21, p->t);
        ui_text_sh(&m->ui, F_M, x + 16, ry, ui_str(&m->ui, lbl[i]), i == p->cur ? MC_SEL : MC_TEXT, UI_LEFT);
    }
    if (m->depth == 1) mi_help(m, ui_str(&m->ui, MS_HELP_SELECT));
}

static void levelup_draw(Menu *m, MenuPage *p)
{
    const MenuGameView *v = m->view;
    const float x = 110, y = 30, w = 260, h = 220;
    mi_panel(m, x, y, w, h);
    ui_text_sh(&m->ui, F_M, x + w / 2, y + 4, ui_str(&m->ui, MS_LEVELUP), MC_GOLD, UI_CENTER);
    float ly = y + 26;
    const float lx = x + 14, lw = w - 28;
    int cost = level_cost(m->lv_level);
    kv(m, lx, ly, lw, ui_str(&m->ui, MS_LEVEL), m->lv_level != v->level ? mi_fmt("%d → %d", v->level, m->lv_level)
       : mi_fmt("%d", v->level), m->lv_level != v->level ? MC_UP : MC_VAL); ly += 14;
    kv(m, lx, ly, lw, ui_str(&m->ui, MS_HAVE), mi_fmt("%d", m->lv_souls), MC_VAL); ly += 14;
    kv(m, lx, ly, lw, ui_str(&m->ui, MS_NEED), mi_fmt("%d", cost), m->lv_souls >= cost ? MC_VAL : 0xff4040c0); ly += 18;
    ui_rect(lx, ly - 4, lw, 1, 0x60607080);
    for (int i = 0; i <= MA_N; i++) {
        if (i == p->cur) mi_cursor(m, x + 4, ly - 2, w - 8, 15, p->t);
        if (i == MA_N) {
            ui_text_sh(&m->ui, F_S, x + w / 2, ly + 2, ui_str(&m->ui, MS_DONE),
                       m->lv_level != v->level ? (i == p->cur ? MC_SEL : MC_TEXT) : MC_DIM, UI_CENTER);
            break;
        }
        int up = m->lv_attr[i] != v->attr[i];
        kv(m, lx, ly, lw, ui_str(&m->ui, MS_VIT + i), mi_fmt("%d", m->lv_attr[i]), up ? MC_UP : MC_VAL);
        if (i == p->cur) {
            ui_text(&m->ui, F_S, lx + lw - 40, ly, "◀", MC_GOLD, UI_CENTER);
            ui_text(&m->ui, F_S, lx + lw + 8, ly, "▶", MC_GOLD, UI_CENTER);
        }
        ly += 15;
    }
    mi_help(m, ui_str(&m->ui, MS_HELP_CHANGE));
    if (p->sub == 1) mi_dialog_draw(m, ui_str(&m->ui, MS_LEVELUP_ASK), p->sub_cur, p->t);
}

static void died_draw(Menu *m)
{
    float t = m->died_t;
    /* DS1: dark band fades in, red letters slowly grow, then everything fades to black [придумал: тайминги] */
    float band = t < 0.6f ? t / 0.6f : 1;
    float ta = t < 0.3f ? 0 : t < 1.6f ? (t - 0.3f) / 1.3f : 1;
    float scale = 0.92f + 0.08f * (t > 3.5f ? 1 : t / 3.5f);
    ui_vgrad(0, 102, UI_W, 34, (u32)(band * 160) << 24, (u32)(band * 200) << 24);
    ui_vgrad(0, 136, UI_W, 34, (u32)(band * 200) << 24, (u32)(band * 160) << 24);
    u32 a = (u32)(ta * 255.0f);
    if (!ui_img(&m->ui, MI_DIED, UI_W / 2, 136, scale, (a << 24) | 0xffffff))
        ui_text_sh(&m->ui, F_L, UI_W / 2, 124, ui_str(&m->ui, MS_DIED), (a << 24) | 0x2020b0, UI_CENTER);
    if (t > 3.8f) {
        float f = (t - 3.8f) / 1.2f;
        ui_rect(0, 0, UI_W, UI_H, (u32)((f > 1 ? 1 : f) * 255.0f) << 24);
    }
}

void game_draw(Menu *m)
{
    /* dim the world under full menus (DS1 keeps the world running behind) */
    int scr = menu_screen(m);
    if (scr == MENU_INVENTORY || scr == MENU_EQUIP || scr == MENU_STATUS || scr == MENU_LEVELUP ||
        (scr == MENU_SETTINGS && m->stack[0].screen == MENU_PAUSE))
        ui_rect(0, 0, UI_W, UI_H, 0x90000000);
    for (int i = 0; i < m->depth; i++) {
        MenuPage *p = &m->stack[i];
        int is_top = i == m->depth - 1;
        switch (p->screen) {
        case MENU_PAUSE: if (is_top) pause_draw(m, p); break;
        case MENU_INVENTORY: inventory_draw(m, p); break;
        case MENU_EQUIP: equip_draw(m, p); break;
        case MENU_STATUS: status_draw(m, p); break;
        case MENU_SETTINGS: settings_draw(m, p); break;
        case MENU_BONFIRE: if (is_top) bonfire_draw(m, p); break;
        case MENU_LEVELUP: levelup_draw(m, p); break;
        case MENU_DIED: died_draw(m); break;
        }
    }
}
