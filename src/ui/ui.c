/* Ash 2D layer: see ui.h. */
#include <pspkernel.h>
#include <pspgu.h>
#include <math.h>
#include <string.h>

#include "ui.h"

typedef struct { u32 color; s16 x, y, z, pad; } CVert;                 /* GU_COLOR_8888 | GU_VERTEX_16BIT */
typedef struct { u16 u, v; u32 color; s16 x, y, z, pad; } TVert;       /* + GU_TEXTURE_16BIT */
typedef struct { float u, v; u32 color; float x, y, z; } FVert;        /* GU_TEXTURE_32BITF | GU_COLOR_8888 | 32BITF */

#define CV_TYPE (GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D)
#define TV_TYPE (GU_TEXTURE_16BIT | GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D)
#define FV_TYPE (GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D)
#define CF_TYPE (GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D)

static u32 __attribute__((aligned(16))) g_font_clut[16];
static int g_tex_state;   /* 0 none, 1 font bound, 2 image bound */

int ui_load(UiRes *r, u8 *file, int size)
{
    memset(r, 0, sizeof(*r));
    if (size < (int)sizeof(UiHdr)) return -1;
    const UiHdr *h = (const UiHdr *)file;
    if (memcmp(h->magic, "ASHU", 4) != 0 || h->version != 1) return -2;
    if (h->off_atlas + (u32)h->atlas_w * h->atlas_h / 2 > (u32)size || h->off_img + 16 * h->n_img > (u32)size) return -3;
    r->file = file;
    r->h = h;
    r->font = (const UiFontRec *)(file + h->off_font);
    r->glyph = (const UiGlyph *)(file + h->off_glyph);
    r->str = (const u32 *)(file + h->off_str);
    r->img = (const UiImgRec *)(file + h->off_img);
    for (int i = 0; i < 16; i++) g_font_clut[i] = ((u32)(i * 17) << 24) | 0x00ffffff;
    sceKernelDcacheWritebackRange(g_font_clut, sizeof(g_font_clut));
    sceKernelDcacheWritebackRange(file, size);
    return 0;
}

const char *ui_str(const UiRes *r, int id)
{
    if (!r->h || id < 0 || (u32)id >= r->h->n_str || !r->str[id]) return "";
    return (const char *)(r->file + r->str[id]);
}

int ui_has_img(const UiRes *r, int id)
{
    return r->h && id >= 0 && (u32)id < r->h->n_img && r->img[id].tw != 0;
}

void ui_img_size(const UiRes *r, int id, int *w, int *h)
{
    *w = *h = 0;
    if (ui_has_img(r, id)) { *w = r->img[id].w; *h = r->img[id].h; }
}

void ui_begin(void)
{
    sceGuDisable(GU_DEPTH_TEST);
    sceGuDepthMask(GU_TRUE);       /* no z writes */
    sceGuDisable(GU_CULL_FACE);
    sceGuDisable(GU_FOG);
    sceGuDisable(GU_LIGHTING);
    sceGuDisable(GU_ALPHA_TEST);
    sceGuEnable(GU_BLEND);
    sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
    sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
    sceGuTexWrap(GU_CLAMP, GU_CLAMP);
    sceGuTexScale(1.0f, 1.0f);
    sceGuTexOffset(0.0f, 0.0f);
    g_tex_state = 0;
}

void ui_end(void)
{
    sceGuDisable(GU_BLEND);
    sceGuDepthMask(GU_FALSE);
    sceGuEnable(GU_DEPTH_TEST);
    sceGuEnable(GU_CULL_FACE);
    sceGuEnable(GU_TEXTURE_2D);
    sceGuTexWrap(GU_REPEAT, GU_REPEAT);
    g_tex_state = 0;
}

static s16 ri(float v) { return (s16)floorf(v + 0.5f); }

void ui_rect(float x, float y, float w, float h, u32 c)
{
    if (w <= 0 || h <= 0) return;
    CVert *v = sceGuGetMemory(2 * sizeof(CVert));
    v[0].color = c; v[0].x = ri(x); v[0].y = ri(y); v[0].z = 0;
    v[1].color = c; v[1].x = ri(x + w); v[1].y = ri(y + h); v[1].z = 0;
    sceGuDisable(GU_TEXTURE_2D);
    sceGuDrawArray(GU_SPRITES, CV_TYPE, 2, 0, v);
}

static void quad4(float x, float y, float w, float h, u32 tl, u32 tr, u32 bl, u32 br)
{
    if (w <= 0 || h <= 0) return;
    CVert *v = sceGuGetMemory(4 * sizeof(CVert));
    v[0].color = tl; v[0].x = ri(x);     v[0].y = ri(y);     v[0].z = 0;
    v[1].color = bl; v[1].x = ri(x);     v[1].y = ri(y + h); v[1].z = 0;
    v[2].color = tr; v[2].x = ri(x + w); v[2].y = ri(y);     v[2].z = 0;
    v[3].color = br; v[3].x = ri(x + w); v[3].y = ri(y + h); v[3].z = 0;
    sceGuDisable(GU_TEXTURE_2D);
    sceGuShadeModel(GU_SMOOTH);
    sceGuDrawArray(GU_TRIANGLE_STRIP, CV_TYPE, 4, 0, v);
}

void ui_vgrad(float x, float y, float w, float h, u32 top, u32 bottom) { quad4(x, y, w, h, top, top, bottom, bottom); }
void ui_hgrad(float x, float y, float w, float h, u32 left, u32 right) { quad4(x, y, w, h, left, right, left, right); }

void ui_frame(float x, float y, float w, float h, u32 c)
{
    ui_rect(x, y, w, 1, c);
    ui_rect(x, y + h - 1, w, 1, c);
    ui_rect(x, y + 1, 1, h - 2, c);
    ui_rect(x + w - 1, y + 1, 1, h - 2, c);
}

/* ---------------------------------------------------------------- text */

static u32 utf8_next(const char **ps)
{
    const u8 *s = (const u8 *)*ps;
    u32 c = *s++;
    if (c >= 0xf0 && s[0] && s[1] && s[2]) { c = ((c & 7) << 18) | ((s[0] & 63) << 12) | ((s[1] & 63) << 6) | (s[2] & 63); s += 3; }
    else if (c >= 0xe0 && s[0] && s[1]) { c = ((c & 15) << 12) | ((s[0] & 63) << 6) | (s[1] & 63); s += 2; }
    else if (c >= 0xc0 && s[0]) { c = ((c & 31) << 6) | (s[0] & 63); s += 1; }
    *ps = (const char *)s;
    return c;
}

static const UiGlyph *glyph(const UiRes *r, int font, u32 cp)
{
    if (!r->h || font < 0 || (u32)font >= r->h->n_font) return 0;
    const UiFontRec *f = &r->font[font];
    int lo = f->first, hi = f->first + f->n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        u32 m = r->glyph[mid].cp;
        if (m == cp) return &r->glyph[mid];
        if (m < cp) lo = mid + 1; else hi = mid - 1;
    }
    if (cp != '?') return glyph(r, font, '?');
    return 0;
}

static int line_w(const UiRes *r, int font, const char *s, const char **end)
{
    int w = 0;
    while (*s && *s != '\n') {
        const UiGlyph *g = glyph(r, font, utf8_next(&s));
        if (g) w += g->adv;
    }
    if (end) *end = s;
    return w;
}

int ui_text_w(const UiRes *r, int font, const char *s)
{
    int best = 0;
    while (1) {
        const char *e;
        int w = line_w(r, font, s, &e);
        if (w > best) best = w;
        if (!*e) break;
        s = e + 1;
    }
    return best;
}

int ui_line_h(const UiRes *r, int font)
{
    if (!r->h || font < 0 || (u32)font >= r->h->n_font) return 12;
    return r->font[font].line_h;
}

static void bind_font(const UiRes *r)
{
    if (g_tex_state == 1) return;
    sceGuClutMode(GU_PSM_8888, 0, 0x0f, 0);
    sceGuClutLoad(2, g_font_clut);
    sceGuTexMode(GU_PSM_T4, 0, 0, 0);
    sceGuTexImage(0, r->h->atlas_w, r->h->atlas_h, r->h->atlas_w, r->file + r->h->off_atlas);
    sceGuTexFilter(GU_NEAREST, GU_NEAREST);
    sceGuTexFlush();
    g_tex_state = 1;
}

int ui_text(const UiRes *r, int font, float x, float y, const char *s, u32 c, int align)
{
    if (!r->h || !s) return 0;
    const UiFontRec *f = &r->font[font];
    int lines = 0;
    bind_font(r);
    sceGuEnable(GU_TEXTURE_2D);
    while (1) {
        const char *e;
        int w = line_w(r, font, s, &e);
        float px = align == UI_CENTER ? x - w / 2 : align == UI_RIGHT ? x - w : x;
        int n = 0;
        for (const char *p = s; p < e;) { utf8_next(&p); n++; }
        if (n > 0) {
            TVert *v = sceGuGetMemory(2 * n * sizeof(TVert));
            int k = 0;
            int pen = ri(px), base = ri(y) + f->ascent + lines * f->line_h;
            for (const char *p = s; p < e;) {
                const UiGlyph *g = glyph(r, font, utf8_next(&p));
                if (!g) continue;
                if (g->w) {
                    TVert *a = &v[k++], *b = &v[k++];
                    a->u = g->x; a->v = g->y; a->color = c; a->x = pen + g->xoff; a->y = base + g->yoff; a->z = 0;
                    b->u = g->x + g->w; b->v = g->y + g->h; b->color = c;
                    b->x = a->x + g->w; b->y = a->y + g->h; b->z = 0;
                }
                pen += g->adv;
            }
            if (k) sceGuDrawArray(GU_SPRITES, TV_TYPE, k, 0, v);
        }
        lines++;
        if (!*e) break;
        s = e + 1;
    }
    return lines * f->line_h;
}

int ui_text_sh(const UiRes *r, int font, float x, float y, const char *s, u32 c, int align)
{
    u32 a = c >> 24;
    ui_text(r, font, x + 1, y + 1, s, ((a * 3 / 4) << 24) | 0x00080808, align);
    return ui_text(r, font, x, y, s, c, align);
}

/* ---------------------------------------------------------------- images */

int ui_img(const UiRes *r, int id, float cx, float cy, float scale, u32 c)
{
    if (!ui_has_img(r, id)) return 0;
    const UiImgRec *im = &r->img[id];
    sceGuClutMode(GU_PSM_8888, 0, 0xff, 0);
    sceGuClutLoad(32, r->file + im->off_clut);
    sceGuTexMode(GU_PSM_T8, 0, 0, 0);
    sceGuTexImage(0, im->tw, im->th, im->tw, r->file + im->off_pix);
    sceGuTexFilter(scale == 1.0f ? GU_NEAREST : GU_LINEAR, scale == 1.0f ? GU_NEAREST : GU_LINEAR);
    sceGuTexFlush();
    g_tex_state = 2;
    float w = im->w * scale, h = im->h * scale;
    FVert *v = sceGuGetMemory(2 * sizeof(FVert));
    v[0].u = 0; v[0].v = 0; v[0].color = c; v[0].x = floorf(cx - w / 2 + 0.5f); v[0].y = floorf(cy - h / 2 + 0.5f); v[0].z = 0;
    v[1].u = im->w; v[1].v = im->h; v[1].color = c; v[1].x = v[0].x + w; v[1].y = v[0].y + h; v[1].z = 0;
    sceGuEnable(GU_TEXTURE_2D);
    sceGuDrawArray(GU_SPRITES, FV_TYPE, 2, 0, v);
    return 1;
}

/* ---------------------------------------------------------------- 3D helpers */

int ui_project(const ScePspFMatrix4 *vp, const float p[3], float *sx, float *sy, float *depth)
{
    float cx = vp->x.x * p[0] + vp->y.x * p[1] + vp->z.x * p[2] + vp->w.x;
    float cy = vp->x.y * p[0] + vp->y.y * p[1] + vp->z.y * p[2] + vp->w.y;
    float cw = vp->x.w * p[0] + vp->y.w * p[1] + vp->z.w * p[2] + vp->w.w;
    if (cw < 0.1f) return 0;
    *sx = (cx / cw * 0.5f + 0.5f) * UI_W;
    *sy = (0.5f - cy / cw * 0.5f) * UI_H;
    if (depth) *depth = cw;
    return 1;
}

typedef struct { u32 color; float x, y, z; } CFVert;

void ui_glow(float cx, float cy, float radius, u32 c)
{
    enum { N = 16 };
    CFVert *v = sceGuGetMemory((N + 2) * sizeof(CFVert));
    v[0].color = c; v[0].x = cx; v[0].y = cy; v[0].z = 0;
    for (int i = 0; i <= N; i++) {
        float a = i * (6.2831853f / N);
        v[i + 1].color = c & 0x00ffffff;
        v[i + 1].x = cx + cosf(a) * radius;
        v[i + 1].y = cy + sinf(a) * radius * 0.8f;
        v[i + 1].z = 0;
    }
    sceGuDisable(GU_TEXTURE_2D);
    sceGuShadeModel(GU_SMOOTH);
    sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_FIX, 0, 0xffffff);
    sceGuDrawArray(GU_TRIANGLE_FAN, CF_TYPE, N + 2, 0, v);
    sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
}
