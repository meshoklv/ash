/*
 * Ash 2D layer for the HUD and menus (sceGu sprites after the 3D pass): `.ashui` resources (tools/cook_ui.py) =
 * 4-bit alpha font atlas (DejaVu Serif, Latin + Cyrillic, 2 sizes), UTF-8 strings from the DSR FMG (Russian),
 * banner / icon textures (8-bit CLUT) cut from the DSR menu atlases. Colours are GE ABGR: 0xAABBGGRR.
 *
 *   UiRes ui;  ui_load(&ui, file, size);              // file stays alive (64-byte aligned, ash_load_file)
 *   ... 3D ...
 *   ui_begin();                                        // inside the same display list, after the 3D pass
 *   ui_text(&ui, UI_FONT_S, 10, 10, ui_str(&ui, S_REST), 0xffffffff, UI_LEFT);
 *   ui_end();                                          // restores depth test + culling, blending off
 * Docs: docs/progress-ui.md.
 */
#ifndef ASH_UI_H
#define ASH_UI_H

#include <psptypes.h>
#include <pspgu.h>
#include "uistr.h"

typedef struct {
    char magic[4];
    u32 version;
    u16 atlas_w, atlas_h;
    u32 off_atlas, n_font, off_font, n_glyph, off_glyph, n_str, off_str, n_img, off_img, pad[4]; /* 64 B */
} UiHdr;

typedef struct { u16 line_h, ascent, first, n; } UiFontRec;
typedef struct { u16 cp, x, y; u8 w, h; s8 xoff, yoff; u8 adv, pad; } UiGlyph;
typedef struct { u16 tw, th, w, h; u32 off_clut, off_pix; } UiImgRec;

typedef struct {
    u8 *file;
    const UiHdr *h;
    const UiFontRec *font;
    const UiGlyph *glyph;
    const u32 *str;
    const UiImgRec *img;
} UiRes;

enum { UI_LEFT = 0, UI_CENTER = 1, UI_RIGHT = 2 };
enum { UI_FONT_S = 0, UI_FONT_L = 1 };

#define UI_W 480
#define UI_H 272

int ui_load(UiRes *r, u8 *file, int size);               /* 0 ok, <0 bad file */
const char *ui_str(const UiRes *r, int id);              /* "" when missing */
int ui_has_img(const UiRes *r, int id);
void ui_img_size(const UiRes *r, int id, int *w, int *h);

void ui_begin(void);
void ui_end(void);

void ui_rect(float x, float y, float w, float h, u32 abgr);
/* vertical gradient: top colour -> bottom colour */
void ui_vgrad(float x, float y, float w, float h, u32 top, u32 bottom);
/* horizontal gradient: left -> right */
void ui_hgrad(float x, float y, float w, float h, u32 left, u32 right);
/* 1-px frame */
void ui_frame(float x, float y, float w, float h, u32 abgr);

/* Text: y = top of the line box; '\n' starts a new line. Returns the height drawn. */
int ui_text(const UiRes *r, int font, float x, float y, const char *utf8, u32 abgr, int align);
/* Same with a 1-px dark drop shadow (readable over the scene). */
int ui_text_sh(const UiRes *r, int font, float x, float y, const char *utf8, u32 abgr, int align);
int ui_text_w(const UiRes *r, int font, const char *utf8);   /* width of the widest line */
int ui_line_h(const UiRes *r, int font);

/* Image centred at (cx, cy), scaled, tinted (modulate). Returns 0 if the image is not in the file. */
int ui_img(const UiRes *r, int id, float cx, float cy, float scale, u32 abgr);

/* World point -> screen (vp = projection * view). 0 if behind the camera. */
int ui_project(const ScePspFMatrix4 *vp, const float p[3], float *sx, float *sy, float *depth);
/* Soft additive glow (no texture: a stack of fading quads) - bloodstain / bonfire marker. */
void ui_glow(float cx, float cy, float radius, u32 abgr);

#endif
