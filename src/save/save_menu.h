/*
 * SaveGame <-> src/menu (menu.h: MenuChar, MenuSettings, MenuGameView). Header-only, so ash_save itself needs no
 * menu / GU headers; include it where both are used (the game, src/save/test/continue).
 */
#ifndef ASH_SAVE_MENU_H
#define ASH_SAVE_MENU_H

#include <string.h>
#include "menu.h"
#include "savegame.h"

/* character from the creation screen (MEV_NEW_GAME: m->chr) */
static inline void sg_from_menu_char(SaveGame *s, const MenuChar *c)
{
    memset(s->name, 0, sizeof(s->name));
    for (int i = 0; i < SG_NAME_BYTES - 1 && i < (int)sizeof(c->name) && c->name[i]; i++) s->name[i] = c->name[i];
    s->class_row = (uint16_t)c->class_row;
    s->gender = (uint8_t)c->gender;
    s->gift = (uint8_t)c->gift_idx;
    s->hair = (uint8_t)c->hair;
    s->level = (uint16_t)c->level;
    s->humanity = c->humanity;
    memcpy(s->attr, c->attr, MA_N);
}

/* MEV_CONTINUE: back into the menu's character (class index = CharaInitParam row - 3000) */
static inline void sg_to_menu_char(const SaveGame *s, MenuChar *c)
{
    memset(c, 0, sizeof(*c));
    for (int i = 0; i < (int)sizeof(c->name) - 1 && i < SG_NAME_BYTES && s->name[i]; i++) c->name[i] = s->name[i];
    c->class_row = s->class_row;
    c->class_idx = s->class_row >= 3000 && s->class_row <= 3009 ? s->class_row - 3000 : 0;
    c->gender = s->gender;
    c->gift_idx = s->gift;
    c->hair = s->hair;
    c->level = s->level;
    c->humanity = s->humanity;
    memcpy(c->attr, s->attr, MA_N);
}

static inline void sg_from_settings(SaveGame *s, const MenuSettings *m)
{
    s->scheme = (int8_t)m->scheme; s->brightness = (int8_t)m->brightness; s->cam_speed = (int8_t)m->cam_speed;
    s->invert_x = (int8_t)m->invert_x; s->invert_y = (int8_t)m->invert_y; s->overlay = (int8_t)m->overlay;
}

static inline void sg_to_settings(const SaveGame *s, MenuSettings *m)
{
    m->scheme = s->scheme; m->brightness = s->brightness; m->cam_speed = s->cam_speed;
    m->invert_x = s->invert_x; m->invert_y = s->invert_y; m->overlay = s->overlay;
}

/* status numbers the save knows over the class defaults of menu_fill_view_from_char (call it first) */
static inline void sg_to_view(const SaveGame *s, MenuGameView *v)
{
    v->level = s->level;
    v->souls = s->souls;
    v->humanity = s->humanity;
    memcpy(v->attr, s->attr, MA_N);
    if (s->hp_max > 0) { v->hp = s->hp; v->hp_max = s->hp_max; }
}

#endif
