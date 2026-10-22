/* src/menu internals shared by menu.c / menu_create.c / menu_game.c (not for the game). */
#ifndef ASH_MENU_INT_H
#define ASH_MENU_INT_H

#include <pspctrl.h>
#include "menu.h"

/* DS1-like palette, GE ABGR [придумал: по памяти о меню DS1] */
#define MC_TEXT   0xffa8bcc4u   /* grey-gold item text */
#define MC_SEL    0xffe4f0f6u   /* selected item */
#define MC_GOLD   0xff70b8dcu   /* headers */
#define MC_DIM    0xff586068u   /* disabled */
#define MC_VAL    0xffd0dce4u   /* values */
#define MC_UP     0xffffc070u   /* raised stat (level up) - light blue */
#define MC_BORDER 0xff3a5468u
#define MC_SHADE  0xd0080808u

enum { B_UP = PSP_CTRL_UP, B_DOWN = PSP_CTRL_DOWN, B_LEFT = PSP_CTRL_LEFT, B_RIGHT = PSP_CTRL_RIGHT,
       B_OK = PSP_CTRL_CROSS, B_BACK = PSP_CTRL_CIRCLE, B_TRI = PSP_CTRL_TRIANGLE, B_SQ = PSP_CTRL_SQUARE,
       B_START = PSP_CTRL_START, B_SELECT = PSP_CTRL_SELECT, B_L = PSP_CTRL_LTRIGGER, B_R = PSP_CTRL_RTRIGGER };

#define F_S 0   /* DejaVu Serif 12 */
#define F_M 1   /* EB Garamond 15 */
#define F_L 2   /* EB Garamond 22 */

static inline MenuPage *top(Menu *m) { return &m->stack[m->depth - 1]; }
void mi_push(Menu *m, int screen);
int mi_pop(Menu *m);                         /* returns 1 if the stack became empty */
int mi_nav(Menu *m, int *cur, int n, int wrap);   /* up/down; returns 1 if moved */
int mi_lr(Menu *m);                          /* -1 / 0 / +1 from left/right (with repeat) */

/* drawing (inside ui_begin .. ui_end) */
void mi_img(const Menu *m, int img, float x, float y, float w, float h, u32 col);
void mi_img_tile(const Menu *m, int img, float x, float y, float w, float h, u32 col);
void mi_panel(const Menu *m, float x, float y, float w, float h);
void mi_cursor(const Menu *m, float x, float y, float w, float h, float t);
void mi_header(const Menu *m, float x, float y, float w, const char *s);
void mi_help(const Menu *m, const char *s);
void mi_bar(float x, float y, float w, float h, float frac, u32 col);
int mi_dialog(Menu *m, const char *q, int *cur);   /* tick: 1 = yes, -1 = no/back, 0 = open */
void mi_dialog_draw(const Menu *m, const char *q, int cur, float t);
void mi_slider(float x, float y, float w, int v, int lo, int hi);
const char *mi_fmt(const char *fmt, ...);   /* small rotating static buffers */
/* Word-wrap UTF-8 text to `width` px (keeps the FMG line breaks), at most max_lines lines; static buffer. */
const char *mi_wrap(const Menu *m, int font, const char *s, int width, int max_lines);
void mi_bg_title(const Menu *m, float alpha);

/* screens */
int create_tick(Menu *m, float dt);
void create_draw(Menu *m);
void create_begin(Menu *m);
void preview_sync(Menu *m);
void preview_draw(Menu *m, float cx, float feet_y, float scale);
int game_tick(Menu *m, float dt);
void game_draw(Menu *m);
void game_begin(Menu *m, int screen);

#endif
