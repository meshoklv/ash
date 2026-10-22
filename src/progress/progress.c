/* Ash progression: see progress.h. Plain C99. */
#include <math.h>
#include <string.h>

#include "progress.h"
#include "uistr.h"

const char *const prg_anim_name[PRG_ANIM_COUNT] = {
    "", "drink_in", "drink", "drink_out", "drink_empty", "sit", "sit_loop", "stand", "death",
};

/* Clip lengths as cooked (tools/cook_progress.py, 30 fps, (frames - 1) / 30) - used until the game sets the real
 * ones with prg_set_clip_len(). */
static const float DEFAULT_LEN[PRG_ANIM_COUNT] = {
    0, 25 / 30.0f, 30 / 30.0f, 40 / 30.0f, 70 / 30.0f, 65 / 30.0f, 60 / 30.0f, 55 / 30.0f, 200 / 30.0f,
};

void prg_default_cfg(PrgCfg *c)
{
    memset(c, 0, sizeof(*c));
    c->estus_max = 5;            /* [версия] Estus from Oscar in the Asylum, unkindled bonfire = 5 */
    c->estus_heal = 300.0f;      /* [проверено] SpEffectParam 3020 HPPointsLost = -300 (Goods 201 ReferenceID) */
    c->heal_time = 0.6f;         /* [версия] HP is added over ~0.6 s, not instantly */
    c->heal_at = 0.30f;          /* [версия] into a00_7586 (31 frames); TAE SpEffect event not decoded */
    c->stain_radius = 1.0f;      /* [версия] touch radius; DS1 has no "retrieve" prompt in the FMG */
    c->banner_in = 0.6f;         /* [версия] by eye from DS1 */
    c->banner_hold = 2.6f;
    c->banner_out = 1.0f;
    c->death_banner_at = 1.6f;   /* [версия] "ВЫ ПОГИБЛИ" while the body falls */
    c->death_black_at = 5.0f;
    c->death_black_len = 1.0f;
    c->loading_time = 1.5f;      /* [придумал] the reload itself is instant; the screen hides the teleport */
    c->fade_in = 1.0f;
    c->autosave_souls_every = 10.0f;  /* [придумал] DS1 saves all the time; we throttle souls-only saves */
}


void prg_init(Progress *p, const PrgCfg *c)
{
    memset(p, 0, sizeof(*p));
    if (c) p->cfg = *c; else prg_default_cfg(&p->cfg);
    p->own_hp = p->own_hp_max = 1.0f;
    p->hp = &p->own_hp;
    p->hp_max = &p->own_hp_max;
    p->estus = p->cfg.estus_max;
    for (int i = 0; i < PRG_ANIM_COUNT; i++) p->clip_len[i] = DEFAULT_LEN[i];
    p->phase = PRG_PLAY;
    p->banner = -1;
    p->toast = -1;
}

void prg_bind_hp(Progress *p, float *hp, float *hp_max)
{
    p->hp = hp ? hp : &p->own_hp;
    p->hp_max = hp_max ? hp_max : &p->own_hp_max;
}

void prg_set_clip_len(Progress *p, int anim, float s)
{
    if (anim > 0 && anim < PRG_ANIM_COUNT && s > 0) p->clip_len[anim] = s;
}

/* Requests go to the pending set; prg_tick publishes them at its end (hooks called between ticks are not lost). */
static void play(Progress *p, int anim, int loop)
{
    p->req_pending |= PRG_REQ_ANIM;
    p->anim_pending = anim;
    p->anim_loop_pending = loop;
}

static void set_phase(Progress *p, int phase, int sub)
{
    p->phase = phase;
    p->sub = sub;
    p->t = 0;
}

int prg_locked(const Progress *p) { return p->phase != PRG_PLAY; }
int prg_resting(const Progress *p) { return p->phase == PRG_REST; }

void prg_banner(Progress *p, int img)
{
    p->banner = img;
    p->banner_t = 0;
    p->banner_sticky = 0;
}

void prg_toast(Progress *p, int str)
{
    p->toast = str;
    p->toast_t = 0;
}

void prg_add_souls(Progress *p, int n)
{
    if (n <= 0) return;
    p->souls += n;
    if (p->souls > 999999999) p->souls = 999999999;
    p->souls_gain += n;
    p->souls_gain_t = 0;
    p->save_pending = 1;
}

int prg_try_estus(Progress *p)
{
    if (p->phase != PRG_PLAY) return 0;
    if (*p->hp <= 0) return 0;
    if (p->estus > 0) {
        p->estus--;
        p->heal_left = p->cfg.estus_heal;
        p->heal_started = 0;
        set_phase(p, PRG_DRINK, 0);
        play(p, PRG_ANIM_DRINK_IN, 0);
    } else {
        set_phase(p, PRG_DRINK, 3);           /* empty flask: shake, no heal (Goods 200 UseAnimation 254) */
        play(p, PRG_ANIM_DRINK_EMPTY, 0);
    }
    return 1;
}

void prg_interrupt(Progress *p)
{
    if (p->phase != PRG_DRINK) return;
    p->heal_left = 0;
    set_phase(p, PRG_PLAY, 0);
}

void prg_player_died(Progress *p, const float pos[3])
{
    if (p->phase == PRG_DYING || p->phase == PRG_LOADING) return;
    p->death_pos[0] = pos[0]; p->death_pos[1] = pos[1]; p->death_pos[2] = pos[2];
    p->heal_left = 0;
    *p->hp = 0;
    set_phase(p, PRG_DYING, 1);               /* sub 1 = death not applied to souls yet */
    play(p, PRG_ANIM_DEATH, 0);
    p->toast = -1;
}

/* Death consequences (DS1): an older, unrecovered stain is lost; all souls go into the new one. */
static void apply_death(Progress *p)
{
    p->stain_on = p->souls > 0;
    p->stain_pos[0] = p->death_pos[0]; p->stain_pos[1] = p->death_pos[1]; p->stain_pos[2] = p->death_pos[2];
    p->stain_souls = p->souls;
    p->souls = 0;
    p->souls_gain = 0;
    p->deaths++;
}

void prg_on_reload(Progress *p)
{
    if ((p->phase == PRG_DYING || p->phase == PRG_LOADING) && p->sub == 1) {
        apply_death(p);
        p->sub = 0;
    }
    *p->hp = *p->hp_max;
    p->estus = p->cfg.estus_max;
    p->heal_left = 0;
    p->req_pending |= PRG_REQ_SAVE;
    p->since_save = 0;
    p->save_pending = 0;
}

void prg_rest_begin(Progress *p)
{
    if (p->phase == PRG_DYING || p->phase == PRG_LOADING || p->phase == PRG_REST) return;
    p->heal_left = 0;
    set_phase(p, PRG_REST, 0);
    play(p, PRG_ANIM_SIT, 0);
    p->menu_n = 0;
    p->menu_item[p->menu_n] = S_LEVELUP; p->menu_enabled[p->menu_n++] = 0;  /* no levelling in the S1 slice */
    p->menu_item[p->menu_n] = S_LEAVE;   p->menu_enabled[p->menu_n++] = 1;
    p->menu_cursor = 1;
}

static void menu_move(Progress *p, int d)
{
    int c = p->menu_cursor;
    for (int i = 0; i < p->menu_n; i++) {
        c = (c + d + p->menu_n) % p->menu_n;
        if (p->menu_enabled[c]) { p->menu_cursor = c; return; }
    }
}

static float dist3(const float a[3], const float b[3])
{
    float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return sqrtf(dx * dx + dy * dy + dz * dz);
}

static void tick_heal(Progress *p, float dt)
{
    if (p->heal_left <= 0) return;
    float rate = p->cfg.estus_heal / (p->cfg.heal_time > 0.01f ? p->cfg.heal_time : 0.01f);
    float add = rate * dt;
    if (add > p->heal_left) add = p->heal_left;
    p->heal_left -= add;
    *p->hp += add;
    if (*p->hp > *p->hp_max) { *p->hp = *p->hp_max; p->heal_left = 0; }
    p->req_pending |= PRG_REQ_HEALED;
}

void prg_tick(Progress *p, const PrgInput *in, float dt)
{
    const PrgCfg *c = &p->cfg;
    p->t += dt;
    p->since_save += dt;

    /* banner / toast / souls gain timers */
    if (p->banner >= 0) {
        p->banner_t += dt;
        if (!p->banner_sticky && p->banner_t > c->banner_in + c->banner_hold + c->banner_out) p->banner = -1;
    }
    if (p->toast >= 0 && (p->toast_t += dt) > 3.0f) p->toast = -1;
    if (p->souls_gain && (p->souls_gain_t += dt) > 2.5f) p->souls_gain = 0;

    switch (p->phase) {
    case PRG_PLAY:
        if (in && in->item && !in->busy) prg_try_estus(p);
        break;
    case PRG_DRINK:
        if (p->sub == 0 && p->t >= p->clip_len[PRG_ANIM_DRINK_IN]) {
            set_phase(p, PRG_DRINK, 1);
            play(p, PRG_ANIM_DRINK, 0);
        } else if (p->sub == 1) {
            if (!p->heal_started && p->t >= c->heal_at) p->heal_started = 1;
            if (p->t >= p->clip_len[PRG_ANIM_DRINK]) { set_phase(p, PRG_DRINK, 2); play(p, PRG_ANIM_DRINK_OUT, 0); }
        } else if (p->sub == 2 && p->t >= p->clip_len[PRG_ANIM_DRINK_OUT] * 0.75f) {   /* [версия] recovery */
            set_phase(p, PRG_PLAY, 0);
        } else if (p->sub == 3 && p->t >= p->clip_len[PRG_ANIM_DRINK_EMPTY] * 0.75f) {
            set_phase(p, PRG_PLAY, 0);
        }
        break;
    case PRG_REST:
        if (p->sub == 0 && p->t >= p->clip_len[PRG_ANIM_SIT]) {
            set_phase(p, PRG_REST, 1);
            play(p, PRG_ANIM_SIT_LOOP, 1);
        } else if (p->sub == 1 && in) {
            if (in->up) menu_move(p, -1);
            if (in->down) menu_move(p, +1);
            if (in->cancel || (in->confirm && p->menu_item[p->menu_cursor] == S_LEAVE)) {
                set_phase(p, PRG_REST, 2);
                play(p, PRG_ANIM_STAND, 0);
            }
        } else if (p->sub == 2 && p->t >= p->clip_len[PRG_ANIM_STAND] * 0.6f) {    /* [версия] free to move */
            set_phase(p, PRG_PLAY, 0);
            p->req_pending |= PRG_REQ_STAND_DONE;
        }
        break;
    case PRG_DYING:
        if (p->t >= c->death_banner_at && p->banner != IMG_DIED) {
            p->banner = IMG_DIED;
            p->banner_t = 0;
            p->banner_sticky = 1;
        }
        if (p->t >= c->death_black_at + c->death_black_len) {
            int sub = p->sub;
            set_phase(p, PRG_LOADING, sub);
            p->req_pending |= PRG_REQ_RELOAD;
        }
        break;
    case PRG_LOADING:
        if (p->t >= c->loading_time && p->sub == 1) {   /* nobody called prg_on_reload (no event module) */
            prg_on_reload(p);
        }
        if (p->t >= c->loading_time + c->fade_in) set_phase(p, PRG_PLAY, 0);
        break;
    }

    /* fade + loading screen */
    p->loading = 0;
    p->fade = 0;
    if (p->phase == PRG_DYING && p->t > c->death_black_at) {
        float f = (p->t - c->death_black_at) / c->death_black_len;
        p->fade = f > 1 ? 1 : f;
    } else if (p->phase == PRG_LOADING) {
        if (p->t < c->loading_time) {
            p->fade = 1;
            p->loading = 1;
            p->banner = -1;
            p->banner_sticky = 0;
        } else {
            float f = 1 - (p->t - c->loading_time) / c->fade_in;
            p->fade = f < 0 ? 0 : f;
        }
    }

    /* Estus heal (also continues into drink_out) */
    if (p->heal_started) tick_heal(p, dt);

    /* bloodstain: touch to retrieve */
    if (p->stain_on && in && (p->phase == PRG_PLAY || p->phase == PRG_DRINK) && *p->hp > 0 &&
        dist3(in->pos, p->stain_pos) <= c->stain_radius) {
        p->stain_on = 0;
        prg_add_souls(p, p->stain_souls);
        p->stain_souls = 0;
        prg_toast(p, S_SOULS_BACK);
        p->req_pending |= PRG_REQ_SOULS_BACK | PRG_REQ_SAVE;
        p->save_pending = 0;
        p->since_save = 0;
    }

    if (p->save_pending && p->since_save >= c->autosave_souls_every && p->phase == PRG_PLAY) {
        p->req_pending |= PRG_REQ_SAVE;
        p->save_pending = 0;
        p->since_save = 0;
    }
    if (p->req_pending & PRG_REQ_SAVE) p->since_save = 0;
    /* publish */
    p->req = p->req_pending;
    p->anim = (p->req & PRG_REQ_ANIM) ? p->anim_pending : PRG_ANIM_NONE;
    p->anim_loop = (p->req & PRG_REQ_ANIM) ? p->anim_loop_pending : 0;
    p->req_pending = 0;
}

/* ---------------------------------------------------------------- save blob */

typedef struct {
    uint32_t version;
    int32_t souls, estus, deaths, stain_on;
    float stain_pos[3];
    int32_t stain_souls;
    uint32_t flags;
    float hp, hp_max;
} PrgBlob;

int prg_save_size(void) { return (int)sizeof(PrgBlob); }

int prg_save_write(const Progress *p, void *buf, int cap)
{
    if (cap < (int)sizeof(PrgBlob)) return -1;
    PrgBlob b;
    memset(&b, 0, sizeof(b));
    b.version = PRG_SAVE_VERSION;
    b.souls = p->souls;
    b.estus = p->estus;
    b.deaths = p->deaths;
    b.stain_on = p->stain_on;
    memcpy(b.stain_pos, p->stain_pos, sizeof(b.stain_pos));
    b.stain_souls = p->stain_souls;
    b.flags = p->flags;
    b.hp = *p->hp;
    b.hp_max = *p->hp_max;
    /* dying / loading: save what the reload will produce (DS1 writes the death immediately) */
    if ((p->phase == PRG_DYING || p->phase == PRG_LOADING) && p->sub == 1) {
        b.stain_on = p->souls > 0;
        memcpy(b.stain_pos, p->death_pos, sizeof(b.stain_pos));
        b.stain_souls = p->souls;
        b.souls = 0;
        b.deaths++;
        b.estus = p->cfg.estus_max;
        b.hp = *p->hp_max;
    }
    memcpy(buf, &b, sizeof(b));
    return (int)sizeof(b);
}

int prg_save_read(Progress *p, const void *buf, int len)
{
    PrgBlob b;
    if (len < (int)sizeof(PrgBlob)) return -1;
    memcpy(&b, buf, sizeof(b));
    if (b.version != PRG_SAVE_VERSION) return -2;
    if (b.souls < 0 || b.estus < 0 || b.stain_souls < 0 || !(b.hp >= 0) || !(b.hp_max > 0)) return -3;
    for (int i = 0; i < 3; i++) if (!(fabsf(b.stain_pos[i]) < 1e5f)) return -3;
    p->souls = b.souls;
    p->estus = b.estus > p->cfg.estus_max ? p->cfg.estus_max : b.estus;
    p->deaths = b.deaths;
    p->stain_on = b.stain_on != 0;
    memcpy(p->stain_pos, b.stain_pos, sizeof(p->stain_pos));
    p->stain_souls = b.stain_souls;
    p->flags = b.flags;
    *p->hp = b.hp > *p->hp_max ? *p->hp_max : b.hp;
    if (*p->hp <= 0) *p->hp = *p->hp_max;
    return 0;
}
