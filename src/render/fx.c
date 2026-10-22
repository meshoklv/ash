/* Sprite particles (render.h "particles"): torch/bonfire fire + glow, hit sparks + flash, roll dust.
 * One pool, CPU update, drawn as GU_SPRITES with 3D transform (2 vertices per particle: opposite corners of a
 * camera-facing square), soft round 32x32 texture made at start. Additive for fire/sparks, alpha for dust. */
#include "render.h"

#include <math.h>
#include <pspgum.h>
#include <pspkernel.h>
#include <string.h>

void render_internal_basis(float right[3], float up[3], float eye[3], float *proj_yy, float *flicker);
int render_cover_begin(void);
void render_cover_end(void);

#define FX_MAX 320
#define FIRE_MAX 32

enum { PK_FIRE, PK_SPARK, PK_FLASH, PK_DUST, PK_EMBER };

typedef struct {
    float p[3], v[3];
    float age, life, s0, s1, drag, grav;
    u32 c0, c1;     /* ABGR at birth / death (alpha included) */
    u8 kind, add;
} Part;

typedef struct { float p[3], size, acc; int bonfire; float seed; } Fire;

static Part g_p[FX_MAX];
static int g_np;
static Fire g_fire[FIRE_MAX];
static int g_nfire;
static u32 g_rng = 0x2545f491;
static u32 __attribute__((aligned(16))) g_tex_dot[32 * 32];
static int g_dot_ready;

static float frand(void)
{
    g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5;
    return (g_rng & 0xffffff) / 16777216.0f;
}
static float srand1(void) { return frand() * 2.0f - 1.0f; }

static void make_dot(void)
{
    for (int y = 0; y < 32; y++)
        for (int x = 0; x < 32; x++) {
            float dx = (x + 0.5f) / 16.0f - 1.0f, dy = (y + 0.5f) / 16.0f - 1.0f;
            float r = sqrtf(dx * dx + dy * dy);
            float a = r >= 1.0f ? 0.0f : (1.0f - r) * (1.0f - r);
            int ai = (int)(a * 255);
            g_tex_dot[y * 32 + x] = ((u32)ai << 24) | 0xffffff;
        }
    sceKernelDcacheWritebackRange(g_tex_dot, sizeof(g_tex_dot));
    g_dot_ready = 1;
}

void fx_reset(void)
{
    g_np = 0;
    g_nfire = 0;
    if (!g_dot_ready) make_dot();
}

static Part *spawn(void)
{
    if (g_np >= FX_MAX) return NULL;
    Part *p = &g_p[g_np++];
    memset(p, 0, sizeof(*p));
    return p;
}

void fx_add_fire(const float pos[3], float size, int bonfire)
{
    if (g_nfire >= FIRE_MAX) return;
    Fire *f = &g_fire[g_nfire++];
    f->p[0] = pos[0]; f->p[1] = pos[1]; f->p[2] = pos[2];
    f->size = size;
    f->acc = 0;
    f->bonfire = bonfire;
    f->seed = frand() * 6.283f;
}

void fx_zone_fires(const RenderZone *z)
{
    if (!z) return;
    for (u32 i = 0; i < z->hdr->n_light; i++) {
        const AshlLight *l = &z->light[i];
        if (l->kind == RL_TORCH) fx_add_fire(l->pos, 0.22f, 0);
        else if (l->kind == RL_BONFIRE) fx_add_fire(l->pos, 0.45f, 1);
    }
}

static void emit_fire(Fire *f, float dt)
{
    float rate = f->bonfire ? 46.0f : 22.0f;     /* particles / s [придумал] */
    f->acc += rate * dt;
    while (f->acc >= 1.0f) {
        f->acc -= 1.0f;
        Part *p = spawn();
        if (!p) return;
        float s = f->size;
        p->p[0] = f->p[0] + srand1() * s * 0.5f;
        p->p[1] = f->p[1] + frand() * s * 0.3f;
        p->p[2] = f->p[2] + srand1() * s * 0.5f;
        p->v[0] = srand1() * 0.15f;
        p->v[1] = 0.7f + frand() * 0.6f;
        p->v[2] = srand1() * 0.15f;
        p->life = 0.45f + frand() * 0.35f;
        p->s0 = s * (0.9f + frand() * 0.5f);
        p->s1 = s * 0.25f;
        p->c0 = 0xff60d8ff;   /* bright yellow-white */
        p->c1 = 0x001040c0;   /* deep red, transparent */
        p->kind = PK_FIRE;
        p->add = 1;
        p->drag = 0.5f;
        if (frand() < 0.06f) {   /* ember: small, long, drifts up */
            Part *e = spawn();
            if (!e) return;
            *e = *p;
            e->s0 = 0.035f; e->s1 = 0.02f;
            e->life = 1.2f + frand();
            e->v[0] = srand1() * 0.4f; e->v[1] = 0.9f + frand() * 0.8f; e->v[2] = srand1() * 0.4f;
            e->c0 = 0xff50b0ff; e->c1 = 0x000830c0;
            e->kind = PK_EMBER;
        }
    }
}

void fx_hit_spark(const float pos[3], const float dir[3], int parry)
{
    int n = parry ? 22 : 14;
    for (int i = 0; i < n; i++) {
        Part *p = spawn();
        if (!p) break;
        p->p[0] = pos[0]; p->p[1] = pos[1]; p->p[2] = pos[2];
        float sp = 3.0f + frand() * 4.0f;
        p->v[0] = dir[0] * sp * 0.6f + srand1() * sp * 0.7f;
        p->v[1] = dir[1] * sp * 0.6f + srand1() * sp * 0.5f + 1.5f;
        p->v[2] = dir[2] * sp * 0.6f + srand1() * sp * 0.7f;
        p->life = 0.22f + frand() * 0.25f;
        p->s0 = 0.06f; p->s1 = 0.025f;
        p->c0 = parry ? 0xffffe8c8 : 0xff80f0ff;
        p->c1 = parry ? 0x00ff8040 : 0x000060ff;
        p->grav = 9.0f;
        p->drag = 1.5f;
        p->kind = PK_SPARK;
        p->add = 1;
    }
    Part *f = spawn();
    if (!f) return;
    f->p[0] = pos[0]; f->p[1] = pos[1]; f->p[2] = pos[2];
    f->life = parry ? 0.18f : 0.12f;
    f->s0 = parry ? 0.9f : 0.6f; f->s1 = parry ? 1.3f : 0.9f;
    f->c0 = parry ? 0xffffe0c0 : 0xffc0f0ff;
    f->c1 = 0x00000000;
    f->kind = PK_FLASH;
    f->add = 1;
}

void fx_roll_dust(const float pos[3], float yaw)
{
    for (int i = 0; i < 9; i++) {
        Part *p = spawn();
        if (!p) break;
        float a = yaw + srand1() * 2.4f + 3.14159f;
        p->p[0] = pos[0] + srand1() * 0.25f;
        p->p[1] = pos[1] + 0.08f + frand() * 0.1f;
        p->p[2] = pos[2] + srand1() * 0.25f;
        float sp = 0.5f + frand() * 0.7f;
        p->v[0] = sinf(a) * sp; p->v[1] = 0.15f + frand() * 0.3f; p->v[2] = cosf(a) * sp;
        p->life = 0.7f + frand() * 0.4f;
        p->s0 = 0.18f; p->s1 = 0.55f + frand() * 0.3f;
        p->c0 = 0x70506070;   /* grey-brown, alpha 0x70 */
        p->c1 = 0x00506070;
        p->drag = 2.2f;
        p->kind = PK_DUST;
        p->add = 0;
    }
}

void fx_update(float dt)
{
    if (!(g_render.flags & RF_FX)) return;
    for (int i = 0; i < g_nfire; i++) emit_fire(&g_fire[i], dt);
    for (int i = 0; i < g_np;) {
        Part *p = &g_p[i];
        p->age += dt;
        if (p->age >= p->life) { g_p[i] = g_p[--g_np]; continue; }
        float d = 1.0f - p->drag * dt;
        if (d < 0) d = 0;
        p->v[0] *= d; p->v[2] *= d;
        p->v[1] = p->v[1] * d - p->grav * dt;
        p->p[0] += p->v[0] * dt; p->p[1] += p->v[1] * dt; p->p[2] += p->v[2] * dt;
        i++;
    }
}

static u32 lerp_col(u32 a, u32 b, float t)
{
    u32 o = 0;
    for (int s = 0; s < 32; s += 8) {
        int ca = (a >> s) & 255, cb = (b >> s) & 255;
        o |= (u32)(ca + (int)((cb - ca) * t)) << s;
    }
    return o;
}

typedef struct { float u, v; u32 color; float x, y, z; } SVert;

static int put(SVert *o, const float c[3], float s, u32 col, const float R[3], const float U[3])
{
    for (int k = 0; k < 2; k++) {
        float sg = k ? 1.0f : -1.0f;
        o[k].u = (float)k; o[k].v = (float)k;
        o[k].color = col;
        o[k].x = c[0] + sg * s * (R[0] + U[0]);
        o[k].y = c[1] + sg * s * (R[1] + U[1]);
        o[k].z = c[2] + sg * s * (R[2] + U[2]);
    }
    return 2;
}

static float px_of(const float c[3], float s, const float eye[3], float pyy)
{
    float dx = c[0] - eye[0], dy = c[1] - eye[1], dz = c[2] - eye[2];
    float d = sqrtf(dx * dx + dy * dy + dz * dz) + 0.05f;
    float w = 2.0f * s * pyy * 136.0f / d;
    return w * w;
}

void render_fx_draw(void)
{
    if (!(g_render.flags & RF_FX) || (g_np == 0 && g_nfire == 0)) return;
    float R[3], U[3], eye[3], pyy, flick;
    render_internal_basis(R, U, eye, &pyy, &flick);
    SVert *v = sceGuGetMemory((g_np + g_nfire) * 2 * sizeof(SVert));
    int n_alpha = 0;
    /* alpha particles (dust) first, then additive */
    for (int i = 0; i < g_np; i++) {
        Part *p = &g_p[i];
        if (p->add) continue;
        float t = p->age / p->life;
        float s = p->s0 + (p->s1 - p->s0) * t;
        n_alpha += put(v + n_alpha, p->p, s, lerp_col(p->c0, p->c1, t), R, U);
        g_render.st.fx_px += (int)px_of(p->p, s, eye, pyy);
    }
    int n_add = 0;
    SVert *va = v + n_alpha;
    for (int i = 0; i < g_nfire; i++) {     /* glow halo, flickering */
        Fire *f = &g_fire[i];
        float k = flick * (0.85f + 0.15f * sinf(f->seed + eye[0] * 0.0f));
        int a = (int)(70 * k);
        float c[3] = {f->p[0], f->p[1] + f->size * 0.6f, f->p[2]};
        float s = f->size * (f->bonfire ? 5.0f : 4.0f) * k;
        n_add += put(va + n_add, c, s, ((u32)(a > 255 ? 255 : a) << 24) | 0x2070d0, R, U);
        g_render.st.fx_px += (int)px_of(c, s, eye, pyy);
    }
    for (int i = 0; i < g_np; i++) {
        Part *p = &g_p[i];
        if (!p->add) continue;
        float t = p->age / p->life;
        float s = p->s0 + (p->s1 - p->s0) * t;
        n_add += put(va + n_add, p->p, s, lerp_col(p->c0, p->c1, t), R, U);
        g_render.st.fx_px += (int)px_of(p->p, s, eye, pyy);
    }
    g_render.st.fx_sprites = (n_alpha + n_add) / 2;
    sceGumMatrixMode(GU_MODEL);
    sceGumLoadIdentity();
    sceGuEnable(GU_BLEND);
    sceGuDepthMask(GU_TRUE);
    sceGuDisable(GU_ALPHA_TEST);
    sceGuDisable(GU_CULL_FACE);
    sceGuTexMode(GU_PSM_8888, 0, 0, 0);
    sceGuTexImage(0, 32, 32, 32, g_tex_dot);
    sceGuTexFilter(GU_LINEAR, GU_LINEAR);
    sceGuTexWrap(GU_CLAMP, GU_CLAMP);
    sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
    sceGuTexFlush();
    const int vt = GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D;
    if (n_alpha) {
        sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
        render_cover_begin();
        sceGumDrawArray(GU_SPRITES, vt, n_alpha, NULL, v);
    }
    if (n_add) {
        sceGuEnable(GU_BLEND);
        sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_FIX, 0, 0xffffffff);
        sceGuFog(g_render.fog.near, g_render.fog.far, 0xff000000);
        render_cover_begin();
        sceGumDrawArray(GU_SPRITES, vt, n_add, NULL, va);
    }
    render_cover_end();
    sceGuTexWrap(GU_REPEAT, GU_REPEAT);
    sceGuEnable(GU_CULL_FACE);
    sceGuDisable(GU_BLEND);
    sceGuDepthMask(GU_FALSE);
    render_fog_apply();
}
