/* Ash event interpreter (see event.h, docs/events.md). Semantics marked [версия] are our reconstruction of DS1
 * EMEVD behaviour, not observed engine code. */
#include "event.h"

#include <math.h>
#include <string.h>

#define MAX_STEPS 512                    /* instructions per instance per tick (guards RESTART loops) */
#define SPEFF_TIME 4.0f                  /* seconds a scripted special effect stays on the player [оценка] */
#define TOUCH_DIST 1.6f                  /* objects / treasures / bonfires interaction reach (m) [оценка] */

/* ------------------------------------------------------------------------------------------------ helpers */

static uint32_t rd_word(const Ev *ev, uint32_t w) { return ev->code[w]; }
static float as_f(uint32_t w) { float f; memcpy(&f, &w, 4); return f; }

static float cmd(Ev *ev, EvCmd *c)
{
    return ev->host.cmd ? ev->host.cmd(ev->host.user, c) : 0.0f;
}

static void logf_(Ev *ev, const char *s)
{
    EvCmd c; memset(&c, 0, sizeof c); c.op = EV_CMD_LOG; c.text = s; cmd(ev, &c);
}

static int ent_index(const Ev *ev, uint32_t id)
{
    int lo = 0, hi = (int)ev->n_ent - 1;
    while (lo <= hi) {
        int m = (lo + hi) >> 1;
        uint32_t v = ev->ent[m].id;
        if (v == id) return m;
        if (v < id) lo = m + 1; else hi = m - 1;
    }
    return -1;
}

const EvfEnt *ev_ent(const Ev *ev, uint32_t id)
{
    int i = ent_index(ev, id);
    return i < 0 ? 0 : &ev->ent[i];
}

const char *ev_text(const Ev *ev, uint32_t key)
{
    int lo = 0, hi = (int)ev->n_str - 1;
    while (lo <= hi) {
        int m = (lo + hi) >> 1;
        uint32_t v = ev->str[m].key;
        if (v == key) return ev->pool + ev->str[m].off;
        if (v < key) lo = m + 1; else hi = m - 1;
    }
    return 0;
}

static int is_volatile(uint32_t id) { return id >= 10000000u && (id / 1000u) % 10u == 5u; } /* [версия] */

static int flag_pos(const Ev *ev, uint32_t id, int *found)
{
    int lo = 0, hi = ev->n_flags;
    while (lo < hi) {
        int m = (lo + hi) >> 1;
        if (ev->flags[m] < id) lo = m + 1; else hi = m;
    }
    *found = lo < ev->n_flags && ev->flags[lo] == id;
    return lo;
}

int ev_flag(const Ev *ev, uint32_t id)
{
    int f; flag_pos(ev, id, &f); return f;
}

void ev_set_flag(Ev *ev, uint32_t id, int on)
{
    int f, p = flag_pos(ev, id, &f);
    if (on && !f) {
        if (ev->n_flags >= EV_MAX_FLAGS) { logf_(ev, "flags full"); return; }
        memmove(&ev->flags[p + 1], &ev->flags[p], (size_t)(ev->n_flags - p) * 4);
        ev->flags[p] = id; ev->n_flags++;
    } else if (!on && f) {
        memmove(&ev->flags[p], &ev->flags[p + 1], (size_t)(ev->n_flags - p - 1) * 4);
        ev->n_flags--;
    }
}

/* PSP-space local frame of an entity: world = pos + Ry(yaw) * (lx, ly, lz) with lx already mirrored */
static void ent_local(const EvfEnt *e, float lx, float ly, float lz, float out[3])
{
    float c = cosf(e->yaw), s = sinf(e->yaw);
    out[0] = e->pos[0] + c * lx + s * lz;
    out[1] = e->pos[1] + ly;
    out[2] = e->pos[2] - s * lx + c * lz;
}

int ev_in_region(const EvfEnt *r, const float p[3])
{
    float dx = p[0] - r->pos[0], dy = p[1] - r->pos[1], dz = p[2] - r->pos[2];
    switch (r->shape) {
    case EVSH_SPHERE:
        return dx * dx + dy * dy + dz * dz <= r->dim[0] * r->dim[0];
    case EVSH_CYLINDER:
        return dy >= 0 && dy <= r->dim[1] && dx * dx + dz * dz <= r->dim[0] * r->dim[0];
    case EVSH_BOX: {
        /* inverse yaw rotation; region boxes: translate = bottom centre [версия, m18_01 floors agree] */
        float c = cosf(r->yaw), s = sinf(r->yaw);
        float lx = c * dx - s * dz, lz = s * dx + c * dz;
        return fabsf(lx) <= r->dim[0] * 0.5f && fabsf(lz) <= r->dim[1] * 0.5f && dy >= -0.25f && dy <= r->dim[2];
    }
    default:
        return dx * dx + dz * dz <= 0.25f && fabsf(dy) < 1.0f;
    }
}

static int chr_pos(Ev *ev, uint32_t id, float p[3], float *yaw)
{
    if (id == EV_PLAYER) {
        memcpy(p, ev->in.pos, 12);
        if (yaw) *yaw = ev->in.yaw;
        return 1;
    }
    if (ev->host.chr_pos) return ev->host.chr_pos(ev->host.user, id, p, yaw);
    return 0;
}

/* position of any entity (region, object, chr, spawn) */
static int ent_pos(Ev *ev, uint32_t id, float p[3], float *yaw)
{
    const EvfEnt *e = ev_ent(ev, id);
    if (e && e->kind == EVK_CHR) {
        if (chr_pos(ev, id, p, yaw)) return 1;
    }
    if (!e) return chr_pos(ev, id, p, yaw);
    memcpy(p, e->pos, 12);
    if (yaw) *yaw = e->yaw;
    return 1;
}

static float hquery(Ev *ev, int what, uint32_t id, uint32_t arg)
{
    if (!ev->host.query) return what == EVQ_HP_RATIO ? 1.0f : 0.0f;
    return ev->host.query(ev->host.user, what, id, arg);
}

static int cmp(int op, float l, float r)
{
    switch (op) {
    case 0: return l == r;
    case 1: return l != r;
    case 2: return l > r;
    case 3: return l < r;
    case 4: return l >= r;
    default: return l <= r;
    }
}

static void offer(Ev *ev, uint32_t key, float dist, uint32_t text)
{
    if (!ev->cand_key || dist < ev->cand_dist) {
        ev->cand_key = key; ev->cand_dist = dist; ev->cand_text_id = text;
    }
}

static int fired(const Ev *ev, uint32_t key) { return ev->fire_key && ev->fire_key == key; }

static float flat_dist(const float a[3], const float b[3])
{
    float dx = a[0] - b[0], dz = a[2] - b[2];
    return sqrtf(dx * dx + dz * dz);
}

/* the player looks at p within `angle` degrees (0 = any) */
static int facing(const Ev *ev, const float p[3], float angle)
{
    if (angle <= 0.0f || angle >= 180.0f) return 1;
    float dx = p[0] - ev->in.pos[0], dz = p[2] - ev->in.pos[2];
    float d = sqrtf(dx * dx + dz * dz);
    if (d < 0.05f) return 1;
    /* game convention: yaw 0 looks along -z, positive yaw turns towards -x (src/game/camera.c) [версия] */
    float fx = -sinf(ev->in.yaw), fz = -cosf(ev->in.yaw);
    float c = (fx * dx + fz * dz) / d;
    return c >= cosf(angle * 3.14159265f / 180.0f);
}

static int has_speff(const Ev *ev, uint32_t id)
{
    for (int i = 0; i < EV_MAX_SPEFF; i++)
        if (ev->speff[i].id == id && ev->speff[i].t > 0) return 1;
    return 0;
}

int ev_has_item(const Ev *ev, uint32_t key)
{
    for (int i = 0; i < ev->n_items; i++)
        if (ev->items[i].key == key && ev->items[i].count) return 1;
    return 0;
}

static void give_item(Ev *ev, uint32_t key, int count, const char *name)
{
    int i;
    for (i = 0; i < ev->n_items; i++)
        if (ev->items[i].key == key) break;
    if (i == ev->n_items) {
        if (ev->n_items >= EV_MAX_ITEMS) return;
        ev->items[ev->n_items].key = key; ev->items[ev->n_items].count = 0; ev->n_items++;
    }
    ev->items[i].count = (uint16_t)(ev->items[i].count + count);
    EvCmd c; memset(&c, 0, sizeof c);
    c.op = EV_CMD_ITEM; c.a = (int32_t)key; c.b = count; c.text = name;
    cmd(ev, &c);
}

static const EvfLot *find_lot(const Ev *ev, uint32_t lot)
{
    for (uint32_t i = 0; i < ev->n_lot; i++)
        if (ev->lot[i].lot_id == lot) return &ev->lot[i];
    return 0;
}

/* award an item lot unless its ItemFlag is already set; returns items given */
static int give_lot(Ev *ev, uint32_t lot_id)
{
    const EvfLot *l = find_lot(ev, lot_id);
    if (!l) return 0;
    if (l->flag && l->flag != 0xFFFFFFFFu && ev_flag(ev, l->flag)) return 0;
    if (l->flag && l->flag != 0xFFFFFFFFu) ev_set_flag(ev, l->flag, 1);
    for (int i = 0; i < l->n; i++) {
        const EvfLotItem *it = &ev->lotitem[l->first + i];
        const char *name = it->str != 0xFFFF ? ev->pool + ev->str[it->str].off : 0;
        give_item(ev, it->item, it->count, name);
    }
    return l->n;
}

/* ------------------------------------------------------------------------------------------------ objects */

static int is_fog(uint32_t m) { return m == 8950 || m == 8952 || m == 8953; }
static int is_door(uint32_t m) { return m == 8540 || m == 8541 || m == 8544 || m == 8542; }

static EvObjRt *obj_rt(Ev *ev, uint32_t id, const EvfEnt **e)
{
    int i = ent_index(ev, id);
    if (i < 0) return 0;
    if (e) *e = &ev->ent[i];
    return &ev->obj[i];
}

static float obj_anim(Ev *ev, uint32_t id, int anim, int instant)
{
    const EvfEnt *e = 0;
    EvObjRt *o = obj_rt(ev, id, &e);
    if (!o) return 0;
    o->anim = (int16_t)anim;
    if (is_door(e->model)) {                 /* doors and the gate: anim 1 opens, anim 3 closes (m18_01 usage) */
        if (anim == 1) o->open = 1;
        if (anim == 3) o->open = 0;
    }
    EvCmd c; memset(&c, 0, sizeof c);
    c.op = EV_CMD_OBJ_ANIM; c.id = id; c.a = anim; c.f[0] = instant ? -1.0f : 0.0f;
    memcpy(c.pos, e->pos, 12); c.yaw = e->yaw;
    return instant ? 0.0f : cmd(ev, &c);
}

static void obj_state_cmd(Ev *ev, uint32_t id)
{
    const EvfEnt *e = 0;
    EvObjRt *o = obj_rt(ev, id, &e);
    if (!o) return;
    EvCmd c; memset(&c, 0, sizeof c);
    c.op = EV_CMD_OBJ_STATE; c.id = id; c.a = o->enabled; c.b = o->destroyed;
    memcpy(c.pos, e->pos, 12); c.yaw = e->yaw;
    cmd(ev, &c);
}

/* interaction point of an object (door centre etc.) */
static void obj_touch_point(const EvfEnt *e, float p[3])
{
    if (e->model == 8542) ent_local(e, 0, 0, 0, p);
    else if (is_door(e->model)) ent_local(e, -0.5f * (e->dim[0] + e->dim[1]), 0, 0, p);
    else memcpy(p, e->pos, 12);
}

static int objact_index(const Ev *ev, uint32_t id)
{
    for (uint32_t i = 0; i < ev->n_objact; i++)
        if (ev->objact[i].id == id) return (int)i;
    return -1;
}

/* ------------------------------------------------------------------------------------------------ native
 * Interactables that DS1 handles in the engine, not in EMEVD: treasures, object activations (doors), ladders,
 * bonfires. Each offers a prompt; a press on the frame after the offer fires it. */

static void native_interactables(Ev *ev)
{
    const float *p = ev->in.pos;
    /* treasures */
    for (uint32_t i = 0; i < ev->n_treas; i++) {
        float tp[3];
        if (!ev_treasure_get(ev, (int)i, tp)) continue;
        float d = flat_dist(p, tp);
        if (d > TOUCH_DIST || fabsf(p[1] - tp[1]) > 1.5f) continue;
        uint32_t key = 0x10000000u | i;
        if (fired(ev, key)) {
            const EvfTreasure *t = &ev->treas[i];
            for (int k = 0; k < 2; k++) if (t->lot[k]) give_lot(ev, t->lot[k]);
            ev->treas_on[i] = 0;
            EvCmd c; memset(&c, 0, sizeof c); c.op = EV_CMD_CHR_ANIM; c.id = EV_PLAYER; c.a = 7500; /* pick up */
            cmd(ev, &c);
            continue;
        }
        offer(ev, key, d, 900001);
    }
    /* object activations (doors) */
    for (uint32_t i = 0; i < ev->n_objact; i++) {
        const EvfObjAct *a = &ev->objact[i];
        const EvfEnt *e = 0;
        EvObjRt *o = obj_rt(ev, a->obj, &e);
        if (!o || !o->enabled || !o->act_enabled || o->open) continue;
        if (a->done_flag && ev_flag(ev, a->done_flag)) continue;
        float tp[3]; obj_touch_point(e, tp);
        float d = flat_dist(p, tp);
        if (d > TOUCH_DIST || fabsf(p[1] - tp[1]) > 1.5f || !facing(ev, tp, 75.0f)) continue;
        uint32_t key = 0x20000000u | i;
        if (fired(ev, key)) {
            int ok = a->need_type == 0 || (a->need_type == 1 && ev_has_item(ev, a->need)) ||
                     (a->need_type == 2 && has_speff(ev, a->need)) ||
                     (a->auto_flag && a->auto_flag != 0xFFFFFFFFu && ev_flag(ev, a->auto_flag));
            EvCmd c; memset(&c, 0, sizeof c);
            c.op = EV_CMD_OBJACT; c.id = a->obj; c.a = (int32_t)a->id; c.b = ok;
            c.text = ok ? 0 : ev_text(ev, a->fail);
            cmd(ev, &c);
            if (ok) {
                if (a->done_flag) ev_set_flag(ev, a->done_flag, 1);
                ev->objact_pulse[i] = 1;
                obj_anim(ev, a->obj, 1, 0);
            }
            continue;
        }
        offer(ev, key, d, a->prompt);
    }
    /* ladders: bottom = dim[0] along local z; top = dim[1] up, dim[2] along z (o8500 dummies 191/192) */
    for (int i = 0; i < ev->n_ladder; i++) {
        const EvfEnt *e = ev_ent(ev, ev->ladder[i].obj);
        if (!e) continue;
        float bot[3], top[3];
        ent_local(e, 0, 0, e->dim[0] - 0.3f, bot);
        ent_local(e, 0, e->dim[1], e->dim[2] + 0.5f, top);
        for (int up = 1; up >= 0; up--) {
            const float *from = up ? bot : top, *to = up ? top : bot;
            float d = flat_dist(p, from);
            if (d > 1.2f || fabsf(p[1] - from[1]) > 1.0f) continue;
            uint32_t key = 0x30000000u | (uint32_t)(i * 2 + up);
            if (fired(ev, key)) {
                ev_set_flag(ev, ev->ladder[i].start_flag, 1);
                EvCmd c; memset(&c, 0, sizeof c);
                c.op = EV_CMD_LADDER; c.id = e->id; c.a = up; memcpy(c.pos, to, 12);
                c.yaw = up ? e->yaw : e->yaw + 3.14159265f;
                cmd(ev, &c);
                ev_set_flag(ev, ev->ladder[i].start_flag, 0);
                ev_set_flag(ev, ev->ladder[i].stop_flag, 1);
                continue;
            }
            offer(ev, key, d, up ? 10010300u : 10010301u);
        }
    }
    /* bonfires */
    for (int i = 0; i < ev->n_bonfire; i++) {
        const EvfEnt *e = ev_ent(ev, ev->bonfire[i].obj);
        if (!e) continue;
        float d = flat_dist(p, e->pos);
        if (d > ev->bonfire[i].dist + 1.2f || fabsf(p[1] - e->pos[1]) > 1.5f) continue;
        int lit = ev_flag(ev, ev->bonfire[i].flag);
        uint32_t key = 0x40000000u | (uint32_t)i;
        if (fired(ev, key)) {
            EvCmd c; memset(&c, 0, sizeof c); c.id = e->id;
            if (!lit) {
                ev_set_flag(ev, ev->bonfire[i].flag, 1);
                c.op = EV_CMD_BONFIRE_LIT; cmd(ev, &c);
            } else {
                if (e->extra) ev->respawn = e->extra;
                c.op = EV_CMD_REST; cmd(ev, &c);
                ev->pending_reload = 1;
            }
            continue;
        }
        offer(ev, key, d, lit ? 10010183u : 10010182u);
    }
}

/* ------------------------------------------------------------------------------------------------ VM */

static int gi(int g) { return g + 7; }

static void grp_add(EvRun *r, int g, int test)
{
    int8_t *v = &r->grp[gi(g)];
    if (g > 0) *v = (int8_t)(*v < 0 ? test : (*v && test));
    else *v = (int8_t)(*v < 0 ? test : (*v || test));
}

/* compiles a group (empty -> false), stores it as the "last result" and clears it */
static int grp_take(EvRun *r, int g)
{
    int v = r->grp[gi(g)] > 0;
    r->last[gi(g)] = (int8_t)v;
    r->grp[gi(g)] = -1;
    return v;
}

static void grp_clear(EvRun *r) { memset(r->grp, 0xFF, sizeof r->grp); }

static uint32_t flag_id(const EvfInst *in, int type, int32_t flag)
{
    if (type == 1) return in->event_id + (uint32_t)flag;
    if (type == 2) return in->event_id + in->slot + (uint32_t)flag;
    return (uint32_t)flag;
}

static void inst_start(Ev *ev, int i)
{
    EvRun *r = &ev->run[i];
    memset(r, 0, sizeof *r);
    grp_clear(r);
    memset(r->last, 0, sizeof r->last);
    r->status = 1;
}

static void inst_end(Ev *ev, int i)
{
    const EvfInst *in = &ev->inst[i];
    ev->run[i].status = 2;
    if (in->event_id >= 10000) ev_set_flag(ev, in->event_id + in->slot, 1);   /* event flag on End [версия] */
}

static void inst_restart(Ev *ev, int i)
{
    EvRun *r = &ev->run[i];
    r->pc = 0; r->t = 0; r->in_cond = 0; r->counter = 0;
    grp_clear(r);
    r->yield = 1;
}

/* evaluates one condition op -> test (and side effects: action-button offers) */
static int cond_test(Ev *ev, int ii, int op, const uint32_t *a)
{
    EvRun *r = &ev->run[ii];
    const EvfInst *in = &ev->inst[ii];
    switch (op) {
    case EVOP_C_CONST: return (int32_t)a[1] != 0;
    case EVOP_C_COND: return grp_take(r, (int32_t)a[2]) == (int32_t)a[1];
    case EVOP_C_TIME: return r->t >= as_f(a[1]);
    case EVOP_C_FLAG: return ev_flag(ev, flag_id(in, (int32_t)a[2], (int32_t)a[3])) == ((int32_t)a[1] != 0);
    case EVOP_C_REGION: {
        const EvfEnt *reg = ev_ent(ev, a[3]);
        float p[3];
        int inside = reg && chr_pos(ev, a[2], p, 0) && ev_in_region(reg, p);
        return inside == ((int32_t)a[1] != 0);
    }
    case EVOP_C_ACTION: {
        /* group, anchor_type, anchor, angle, dist, prompt, line_obj, boss */
        float p[3], d;
        int ok;
        const EvfEnt *anc = ev_ent(ev, a[2]);
        if ((int32_t)a[1] == 1) {                    /* region anchor: be inside it (facing ignored) */
            ok = anc && ev_in_region(anc, ev->in.pos);
            d = anc ? flat_dist(ev->in.pos, anc->pos) : 99.0f;
        } else {
            if (!ent_pos(ev, a[2], p, 0)) return 0;
            if (anc && anc->kind == EVK_OBJECT) obj_touch_point(anc, p);
            float maxd = as_f(a[4]) > 0 ? as_f(a[4]) : TOUCH_DIST;
            d = flat_dist(ev->in.pos, p);
            ok = d <= maxd && fabsf(ev->in.pos[1] - p[1]) < 2.0f && facing(ev, p, as_f(a[3]));
        }
        if (!ok) return 0;
        uint32_t key = 0x50000000u | ((uint32_t)ii << 12) | r->pc;
        if (fired(ev, key)) return 1;
        offer(ev, key, d, a[5]);
        return 0;
    }
    case EVOP_C_DEAD: return (hquery(ev, EVQ_DEAD, a[1], 0) != 0) == ((int32_t)a[2] != 0);
    case EVOP_C_ATTACKED: return hquery(ev, EVQ_ATTACKED, a[1], a[2]) != 0;
    case EVOP_C_HP: return cmp((int32_t)a[2], hquery(ev, EVQ_HP_RATIO, a[1], 0), as_f(a[3]));
    case EVOP_C_OBJ_DESTROYED: {
        EvObjRt *o = obj_rt(ev, a[2], 0);
        return o && o->destroyed == ((int32_t)a[1] != 0);
    }
    case EVOP_C_OBJ_ACTIVATED: {
        int k = objact_index(ev, a[1]);
        return k >= 0 && ev->objact_pulse[k];
    }
    case EVOP_C_ON_COLLISION: {
        const EvfEnt *e = ev_ent(ev, a[1]);
        const char *name = e && e->str != 0xFFFF ? ev->pool + ev->str[e->str].off : 0;
        return name && ev->in.floor_part && strcmp(name, ev->in.floor_part) == 0;
    }
    }
    return 0;
}

static void set_chr(Ev *ev, uint32_t id, int what, int value)
{
    EvCmd c; memset(&c, 0, sizeof c);
    c.op = EV_CMD_CHR_SET; c.id = id; c.a = what; c.b = value;
    cmd(ev, &c);
}

/* one action op; returns seconds to block (0 = continue) or -1 to stop the instance (demo end) */
static float action(Ev *ev, int ii, int op, const uint32_t *a, int n)
{
    EvRun *r = &ev->run[ii];
    EvCmd c; memset(&c, 0, sizeof c);
    (void)n;
    switch (op) {
    case EVOP_RUN:
        if (a[0] < ev->n_inst && ev->run[a[0]].status == 0) inst_start(ev, (int)a[0]);
        return 0;
    case EVOP_FLAG:
        if (a[1] == 2) ev_set_flag(ev, a[0], !ev_flag(ev, a[0]));
        else ev_set_flag(ev, a[0], a[1] != 0);
        return 0;
    case EVOP_FLAG_RANGE:
        for (uint32_t f = a[0]; f <= a[1] && f - a[0] < 4096; f++) ev_set_flag(ev, f, a[2] != 0);
        return 0;
    case EVOP_RESPAWN: ev->respawn = a[0]; return 0;
    case EVOP_SAVE: c.op = EV_CMD_SAVE; cmd(ev, &c); return 0;
    case EVOP_AWARD_LOT: give_lot(ev, a[0]); return 0;
    case EVOP_CHR_SET: set_chr(ev, a[0], (int)a[1], (int)a[2]); return 0;
    case EVOP_KILL: c.op = EV_CMD_KILL; c.id = a[0]; c.a = (int32_t)a[1]; cmd(ev, &c); return 0;
    case EVOP_ANIM: {
        const EvfEnt *e = ev_ent(ev, a[0]);
        float t;
        if (e && e->kind == EVK_OBJECT) t = obj_anim(ev, a[0], (int32_t)a[1], 0);
        else {
            c.op = EV_CMD_CHR_ANIM; c.id = a[0]; c.a = (int32_t)a[1]; c.b = (int32_t)a[2];
            t = cmd(ev, &c);
        }
        return a[3] ? t : 0;
    }
    case EVOP_FACE:
        c.op = EV_CMD_FACE; c.id = a[0];
        if (ent_pos(ev, a[1], c.pos, 0)) cmd(ev, &c);
        return 0;
    case EVOP_MOVE:
        c.op = EV_CMD_MOVE; c.id = a[0];
        if (ent_pos(ev, a[2], c.pos, &c.yaw)) cmd(ev, &c);
        return 0;
    case EVOP_STANDBY: c.op = EV_CMD_STANDBY; c.id = a[0]; c.a = (int32_t)a[1]; cmd(ev, &c); return 0;
    case EVOP_SPEFFECT:
        if (a[0] == EV_PLAYER) {
            int k, freei = -1;
            for (k = 0; k < EV_MAX_SPEFF; k++) {
                if (ev->speff[k].id == a[1]) break;
                if (ev->speff[k].t <= 0 && freei < 0) freei = k;
            }
            if (k == EV_MAX_SPEFF) k = freei;
            if (k >= 0) { ev->speff[k].id = a[1]; ev->speff[k].t = a[2] ? SPEFF_TIME : 0; }
        }
        c.op = EV_CMD_SPEFFECT; c.id = a[0]; c.a = (int32_t)a[1]; c.b = (int32_t)a[2]; cmd(ev, &c);
        return 0;
    case EVOP_NEST:
        c.op = EV_CMD_NEST; c.id = a[0];
        if (ent_pos(ev, a[1], c.pos, &c.yaw)) cmd(ev, &c);
        return 0;
    case EVOP_AI_CMD: c.op = EV_CMD_AI_CMD; c.id = a[0]; c.a = (int32_t)a[1]; c.b = (int32_t)a[2]; cmd(ev, &c); return 0;
    case EVOP_BOSS_BAR:
        c.op = EV_CMD_BOSS_BAR; c.id = a[1]; c.a = (int32_t)a[0]; c.b = (int32_t)a[2];
        c.text = ev_text(ev, (1u << 28) | a[2]);
        cmd(ev, &c);
        return 0;
    case EVOP_KILL_BOSS:
        c.op = EV_CMD_KILL_BOSS; c.id = a[0]; cmd(ev, &c);
        for (uint32_t k = 0; k < ev->n_drop; k++)
            if (ev->drop[k].chr == a[0]) give_lot(ev, ev->drop[k].lot);
        return 0;
    case EVOP_BOSS_START: c.op = EV_CMD_BOSS_START; cmd(ev, &c); return 0;
    case EVOP_OBJ_STATE: {
        EvObjRt *o = obj_rt(ev, a[0], 0);
        if (o) { o->enabled = a[1] != 0; obj_state_cmd(ev, a[0]); }
        return 0;
    }
    case EVOP_OBJ_DESTROY: case EVOP_OBJ_RESTORE: {
        EvObjRt *o = obj_rt(ev, a[0], 0);
        if (o) { o->destroyed = op == EVOP_OBJ_DESTROY; if (!o->destroyed) o->enabled = 1; obj_state_cmd(ev, a[0]); }
        return 0;
    }
    case EVOP_OBJ_END_ANIM: obj_anim(ev, a[0], (int32_t)a[1], 1); return 0;
    case EVOP_OBJ_ACT: {
        EvObjRt *o = obj_rt(ev, a[0], 0);
        if (o) o->act_enabled = a[1] != 0;
        return 0;
    }
    case EVOP_TREASURE:
        for (uint32_t k = 0; k < ev->n_treas; k++)
            if (ev->treas[k].obj == a[0]) ev->treas_on[k] = a[1] != 0;
        return 0;
    case EVOP_TREASURE_ADD:
        if (a[0] < ev->n_treas) ev->treas_on[a[0]] = 1;
        return 0;
    case EVOP_VFX: {
        const EvfEnt *e = 0;
        EvObjRt *o = obj_rt(ev, a[0], &e);
        if (o) o->vfx_on = a[1] != 0;
        c.op = EV_CMD_VFX; c.id = a[0]; c.a = (int32_t)a[1]; c.b = e ? (int32_t)e->model : 0;
        if (e) memcpy(c.pos, e->pos, 12);
        cmd(ev, &c);
        return 0;
    }
    case EVOP_VFX_TEMP:
        c.op = EV_CMD_VFX; c.id = a[1]; c.a = 2; c.b = (int32_t)a[2];
        ent_pos(ev, a[1], c.pos, 0);
        cmd(ev, &c);
        return 0;
    case EVOP_MAPCOL: {
        const EvfEnt *e = 0;
        EvObjRt *o = obj_rt(ev, a[0], &e);
        if (o) o->enabled = a[1] != 0;
        c.op = EV_CMD_MAPCOL; c.id = a[0]; c.a = (int32_t)a[1];
        c.text = e && e->str != 0xFFFF ? ev->pool + ev->str[e->str].off : 0;
        cmd(ev, &c);
        return 0;
    }
    case EVOP_HAZARD: return 0;                     /* replaced by BALL at cook time */
    case EVOP_DIALOG:
        c.op = EV_CMD_DIALOG; c.a = (int32_t)a[0]; c.text = ev_text(ev, a[0]); cmd(ev, &c);
        return 0;
    case EVOP_CUTSCENE: {
        if (a[1]) {                                 /* "and move the player": warp to the region first */
            EvCmd m; memset(&m, 0, sizeof m);
            m.op = EV_CMD_MOVE; m.id = EV_PLAYER;
            if (ent_pos(ev, a[1], m.pos, &m.yaw)) cmd(ev, &m);
        }
        c.op = EV_CMD_CUTSCENE; c.id = a[0]; c.a = (int32_t)a[2]; c.text = ev_text(ev, a[2]);
        return cmd(ev, &c);
    }
    case EVOP_SOUND: c.op = EV_CMD_SOUND; c.id = a[0]; c.a = (int32_t)a[1]; cmd(ev, &c); return 0;
    case EVOP_SFX: c.op = EV_CMD_SFX; c.id = a[0]; c.a = (int32_t)a[1]; c.b = (int32_t)a[2]; cmd(ev, &c); return 0;
    case EVOP_LADDER:
        if (ev->n_ladder < EV_MAX_LADDERS) {
            for (int k = 0; k < ev->n_ladder; k++) if (ev->ladder[k].obj == a[2]) return 0;
            EvLadder *l = &ev->ladder[ev->n_ladder++];
            l->start_flag = a[0]; l->stop_flag = a[1]; l->obj = a[2];
        }
        return 0;
    case EVOP_BONFIRE:
        if (ev->n_bonfire < EV_MAX_BONFIRES) {
            for (int k = 0; k < ev->n_bonfire; k++) if (ev->bonfire[k].obj == a[1]) return 0;
            EvBonfire *b = &ev->bonfire[ev->n_bonfire++];
            b->flag = a[0]; b->obj = a[1]; b->dist = as_f(a[2]);
        }
        return 0;
    case EVOP_END_DEMO:
        ev->demo_end = 1;
        c.op = EV_CMD_END_DEMO; c.a = (int32_t)a[0]; c.text = ev_text(ev, a[0] ? a[0] : 900002u);
        cmd(ev, &c);
        return -1;
    case EVOP_FADE: return as_f(a[0]) + as_f(a[1]);
    case EVOP_BALL: {
        if (r->counter == 0) {                      /* start rolling; this op runs again when the ball arrives */
            c.op = EV_CMD_BALL; c.id = a[0]; c.a = (int32_t)a[1]; c.f[0] = as_f(a[2]); c.f[1] = as_f(a[3]);
            ent_pos(ev, a[0], c.pos, 0); ent_pos(ev, a[1], c.pos2, 0);
            float t = cmd(ev, &c);
            if (t <= 0 && c.f[0] > 0) t = flat_dist(c.pos, c.pos2) / c.f[0];
            r->counter = 1; r->pc--;
            return t > 0 ? t : 0.001f;
        }
        r->counter = 0;                             /* arrived: the wall breaks (11810200 waits for this) */
        EvObjRt *o = obj_rt(ev, a[1], 0);
        if (o) { o->destroyed = 1; obj_state_cmd(ev, a[1]); }
        return 0;
    }
    case EVOP_BOMB: {
        if (hquery(ev, EVQ_DEAD, a[0], 0) != 0) return 0;
        c.op = EV_CMD_BOMB; c.id = a[0]; c.a = (int32_t)a[1];
        if (!chr_pos(ev, a[0], c.pos, 0)) { const EvfEnt *e = ev_ent(ev, a[0]); if (e) memcpy(c.pos, e->pos, 12); }
        chr_pos(ev, a[1], c.pos2, 0);
        cmd(ev, &c);
        if (++r->counter < (int32_t)a[2]) { r->pc--; return as_f(a[3]); }   /* repeat this op after the interval */
        r->counter = 0;
        return 0;
    }
    case EVOP_BANNER: c.op = EV_CMD_BANNER; c.a = (int32_t)a[0]; c.text = ev_text(ev, a[0]); cmd(ev, &c); return 0;
    }
    return 0;
}

static void run_instance(Ev *ev, int ii, float dt)
{
    EvRun *r = &ev->run[ii];
    const EvfInst *in = &ev->inst[ii];
    r->t += dt;
    if (r->wait > 0) { r->wait -= dt; if (r->wait > 0) return; r->wait = 0; }
    if (r->wait_frames > 0) { if (--r->wait_frames > 0) return; }
    r->yield = 0;
    for (int steps = 0; steps < MAX_STEPS; steps++) {
        if (r->pc >= in->n) { inst_end(ev, ii); return; }
        /* locate instruction pc: walk forward from the cached (pc, word) or from the start */
        uint32_t w, k;
        if (r->cpc <= r->pc && r->cw) { w = r->cw; k = r->cpc; } else { w = in->code_off; k = 0; }
        for (; k < r->pc; k++) w += 1 + (rd_word(ev, w) >> 16);
        r->cpc = r->pc; r->cw = w;
        uint32_t head = rd_word(ev, w);
        int op = (int)(head & 0xFFFF), n = (int)(head >> 16);
        const uint32_t *a = &ev->code[w + 1];
        ev->ninstr_last++;
        if (op >= EVOP_C_CONST && op <= EVOP_C_ON_COLLISION) {
            if (!r->in_cond) { r->in_cond = 1; r->cond_start = r->pc; }
            int g = (int32_t)a[0];
            int t = cond_test(ev, ii, op, a);
            if (g == 0) {
                if (!t) {                           /* MAIN failed: rewind to the start of the block, wait */
                    r->pc = r->cond_start; r->in_cond = 0;
                    grp_clear(r);
                    return;
                }
                grp_clear(r);
                r->in_cond = 0;
            } else {
                grp_add(r, g, t);
            }
            r->pc++;
            continue;
        }
        r->in_cond = 0;
        int32_t a0 = n > 0 ? (int32_t)a[0] : 0;
        switch (op) {
        case EVOP_NOP: r->pc++; continue;
        case EVOP_SKIP: r->pc = (uint16_t)(r->pc + 1 + a0); continue;
        case EVOP_SKIP_IF_COND: {
            int v = grp_take(r, (int32_t)a[2]);
            r->pc = (uint16_t)(r->pc + 1 + (v == ((int32_t)a[1] != 0) ? a0 : 0));
            continue;
        }
        case EVOP_RET_IF_COND: {
            int v = grp_take(r, (int32_t)a[2]);
            if (v == ((int32_t)a[1] != 0)) goto do_ret;
            r->pc++;
            continue;
        }
        case EVOP_RET: goto do_ret;
        case EVOP_SKIP_IF_CMP:
            r->pc = (uint16_t)(r->pc + 1 + (cmp((int32_t)a[1], (float)(int32_t)a[2], (float)(int32_t)a[3]) ? a0 : 0));
            continue;
        case EVOP_RET_IF_CMP:
            if (cmp((int32_t)a[1], (float)(int32_t)a[2], (float)(int32_t)a[3])) goto do_ret;
            r->pc++;
            continue;
        case EVOP_SKIP_IF_LAST:
            r->pc = (uint16_t)(r->pc + 1 + ((r->last[gi((int32_t)a[2])] > 0) == ((int32_t)a[1] != 0) ? a0 : 0));
            continue;
        case EVOP_RET_IF_LAST:
            if ((r->last[gi((int32_t)a[2])] > 0) == ((int32_t)a[1] != 0)) goto do_ret;
            r->pc++;
            continue;
        case EVOP_SKIP_IF_FLAG: {
            int f = ev_flag(ev, flag_id(in, (int32_t)a[2], (int32_t)a[3]));
            r->pc = (uint16_t)(r->pc + 1 + (f == ((int32_t)a[1] != 0) ? a0 : 0));
            continue;
        }
        case EVOP_RET_IF_FLAG: {
            int f = ev_flag(ev, flag_id(in, (int32_t)a[2], (int32_t)a[3]));
            if (f == ((int32_t)a[1] != 0)) goto do_ret;
            r->pc++;
            continue;
        }
        case EVOP_WAIT: r->pc++; r->wait = as_f(a[0]); return;
        case EVOP_WAIT_FRAMES: r->pc++; r->wait_frames = a0; return;
        default: {
            r->pc++;
            float t = action(ev, ii, op, a, n);
            if (t < 0) { r->status = 2; return; }   /* END_DEMO: this instance stops for good */
            if (t > 0) { r->wait = t; return; }
            if (r->status != 1) return;
            continue;
        }
        }
    do_ret:
        if (a0 == 1) {
            /* Restart also sets the event flag [версия: 11815395 tests FlagDisabled(11815390) of an event that
             * only ever restarts — the boss bar would flicker on the balcony otherwise] */
            if (in->event_id >= 10000) ev_set_flag(ev, in->event_id + in->slot, 1);
            inst_restart(ev, ii);
            return;
        }
        inst_end(ev, ii);
        return;
    }
    logf_(ev, "event: step limit");
}

/* ------------------------------------------------------------------------------------------------ public */

int ev_mem_size(const uint8_t *file, int size)
{
    const EvfHeader *h = (const EvfHeader *)file;
    if (size < (int)sizeof *h || memcmp(h->magic, "AEVT", 4)) return -1;
    uint32_t n_inst = h->sec[EVS_INST].n, n_ent = h->sec[EVS_ENT].n, n_tr = h->sec[EVS_TREAS].n;
    uint32_t n_oa = h->sec[EVS_OBJACT].n;
    return (int)(n_inst * sizeof(EvRun) + n_ent * sizeof(EvObjRt) + ((n_tr + n_oa + 7) & ~3u) + 16);
}

int ev_load(Ev *ev, const uint8_t *file, int size, void *mem, const EvHost *host)
{
    const EvfHeader *h = (const EvfHeader *)file;
    memset(ev, 0, sizeof *ev);
    if (size < (int)sizeof *h || memcmp(h->magic, "AEVT", 4) || h->version != EVF_VERSION) return -1;
    for (int s = 0; s < EVS_COUNT; s++)
        if (h->sec[s].off > (uint32_t)size || (h->sec[s].off & 3)) return -2;
    ev->h = h;
#define SEC(field, type, s) ev->field = (const type *)(file + h->sec[s].off)
    SEC(inst, EvfInst, EVS_INST); SEC(code, uint32_t, EVS_CODE); SEC(ent, EvfEnt, EVS_ENT);
    SEC(treas, EvfTreasure, EVS_TREAS); SEC(lot, EvfLot, EVS_LOT); SEC(lotitem, EvfLotItem, EVS_LOTITEM);
    SEC(str, EvfStr, EVS_STR); SEC(pool, char, EVS_POOL); SEC(root, uint32_t, EVS_ROOT);
    SEC(objact, EvfObjAct, EVS_OBJACT); SEC(drop, EvfDrop, EVS_DROP);
#undef SEC
    ev->n_inst = h->sec[EVS_INST].n; ev->n_ent = h->sec[EVS_ENT].n; ev->n_treas = h->sec[EVS_TREAS].n;
    ev->n_lot = h->sec[EVS_LOT].n; ev->n_str = h->sec[EVS_STR].n; ev->n_root = h->sec[EVS_ROOT].n;
    ev->n_objact = h->sec[EVS_OBJACT].n; ev->n_drop = h->sec[EVS_DROP].n;
    if (host) ev->host = *host;
    uint8_t *m = (uint8_t *)mem;
    ev->run = (EvRun *)m; m += ev->n_inst * sizeof(EvRun);
    ev->obj = (EvObjRt *)m; m += ev->n_ent * sizeof(EvObjRt);
    ev->treas_on = m; m += ev->n_treas;
    ev->objact_pulse = m;
    return 0;
}

static void world_reset(Ev *ev)
{
    for (uint32_t i = 0; i < ev->n_ent; i++) {
        EvObjRt *o = &ev->obj[i];
        memset(o, 0, sizeof *o);
        o->enabled = 1; o->act_enabled = 1;
    }
    for (uint32_t i = 0; i < ev->n_objact; i++) {     /* doors used before stay open (objact flag) */
        const EvfObjAct *a = &ev->objact[i];
        if (a->done_flag && ev_flag(ev, a->done_flag)) {
            EvObjRt *o = obj_rt(ev, a->obj, 0);
            if (o) { o->open = 1; o->anim = 1; }
        }
        ev->objact_pulse[i] = 0;
    }
    for (uint32_t i = 0; i < ev->n_treas; i++) ev->treas_on[i] = !ev->treas[i].hidden && ev->treas[i].obj != 0;
    /* Ash-authored treasures (obj 0) wait for TREASURE_ADD; MSB treasures without entity are always on */
    for (uint32_t i = 0; i < ev->n_treas; i++)
        if (ev->treas[i].obj == 0 && (int32_t)ev->treas[i].lot[0] >= 0) ev->treas_on[i] = !ev->treas[i].hidden;
    ev->n_ladder = ev->n_bonfire = 0;
    memset(ev->speff, 0, sizeof ev->speff);
    for (uint32_t i = 0; i < ev->n_inst; i++) memset(&ev->run[i], 0, sizeof(EvRun));
    for (uint32_t i = 0; i < ev->n_root; i++) inst_start(ev, (int)ev->root[i]);
    ev->prompt_key = ev->cand_key = ev->fire_key = 0;
}

void ev_start(Ev *ev) { world_reset(ev); }

void ev_player_died(Ev *ev) { ev->pending_reload = 1; }

static void reload(Ev *ev)
{
    for (int i = ev->n_flags - 1; i >= 0; i--)
        if (is_volatile(ev->flags[i])) ev_set_flag(ev, ev->flags[i], 0);
    world_reset(ev);
    EvCmd c; memset(&c, 0, sizeof c); c.op = EV_CMD_RELOAD; cmd(ev, &c);
}

void ev_tick(Ev *ev, float dt, const EvInput *in)
{
    if (ev->pending_reload) { ev->pending_reload = 0; reload(ev); }
    ev->in = *in;
    ev->time += dt; ev->frame++;
    ev->ninstr_last = 0;
    for (int k = 0; k < EV_MAX_SPEFF; k++) if (ev->speff[k].t > 0) ev->speff[k].t -= dt;
    ev->fire_key = in->interact ? ev->prompt_key : 0;
    ev->cand_key = 0; ev->cand_dist = 1e9f; ev->cand_text_id = 0;
    if (!ev->demo_end) {
        native_interactables(ev);
        for (uint32_t i = 0; i < ev->n_inst; i++)
            if (ev->run[i].status == 1) run_instance(ev, (int)i, dt);
    }
    for (uint32_t i = 0; i < ev->n_objact; i++) ev->objact_pulse[i] = 0;
    ev->prompt_key = ev->cand_key; ev->prompt_text_id = ev->cand_text_id; ev->prompt_dist = ev->cand_dist;
}

const char *ev_prompt(const Ev *ev)
{
    if (!ev->prompt_key) return 0;
    const char *s = ev_text(ev, ev->prompt_text_id);
    return s ? s : "?";
}

int ev_respawn(const Ev *ev, float pos[3], float *yaw)
{
    const EvfEnt *e = ev_ent(ev, ev->respawn ? ev->respawn : 1812900u);
    if (!e) return 0;
    memcpy(pos, e->pos, 12);
    if (yaw) *yaw = e->yaw;
    return 1;
}

int ev_obj_get(const Ev *ev, int i, EvObjInfo *o)
{
    if (i < 0 || (uint32_t)i >= ev->n_ent || ev->ent[i].kind != EVK_OBJECT) return 0;
    const EvfEnt *e = &ev->ent[i];
    const EvObjRt *r = &ev->obj[i];
    o->id = e->id; o->model = e->model; memcpy(o->pos, e->pos, 12); o->yaw = e->yaw; memcpy(o->dim, e->dim, 12);
    o->enabled = r->enabled; o->open = r->open; o->destroyed = r->destroyed; o->anim = r->anim;
    return 1;
}

int ev_treasure_get(const Ev *ev, int i, float pos[3])
{
    if (i < 0 || (uint32_t)i >= ev->n_treas || !ev->treas_on[i]) return 0;
    const EvfTreasure *t = &ev->treas[i];
    if (t->obj) {
        int k = ent_index(ev, t->obj);
        if (k >= 0 && !ev->obj[k].enabled) return 0;
    }
    int any = 0;
    for (int j = 0; j < 2; j++) {
        if (!t->lot[j]) continue;
        const EvfLot *l = find_lot(ev, t->lot[j]);
        if (l && !(l->flag && l->flag != 0xFFFFFFFFu && ev_flag(ev, l->flag))) any = 1;
    }
    if (!any) return 0;
    memcpy(pos, t->pos, 12);
    return 1;
}

int ev_push_out(const Ev *ev, float pos[3], float r, int ignore_fog)
{
    int hit = 0;
    for (uint32_t i = 0; i < ev->n_ent; i++) {
        const EvfEnt *e = &ev->ent[i];
        const EvObjRt *o = &ev->obj[i];
        if (e->kind != EVK_OBJECT || !o->enabled || o->destroyed) continue;
        float cx, cz, hx, hz;                      /* box in the object's local frame (mirrored x) */
        if (is_fog(e->model)) {
            if (ignore_fog) continue;
            cx = 0; hx = 0.5f * (e->dim[1] - e->dim[0]); cz = 0; hz = 0.15f;
        } else if (e->model == 8542) {             /* gate: spans local z */
            if (o->open) continue;
            cx = 0; hx = 0.15f; cz = 0.5f * (e->dim[0] + e->dim[1]); hz = 0.5f * (e->dim[1] - e->dim[0]);
        } else if (is_door(e->model)) {
            if (o->open) continue;
            cx = -0.5f * (e->dim[0] + e->dim[1]); hx = 0.5f * (e->dim[1] - e->dim[0]); cz = 0; hz = 0.15f;
        } else if (e->model == 8520) {             /* wall broken by the iron ball */
            cx = 0; hx = 0.5f * (e->dim[1] - e->dim[0]); cz = 0.1f; hz = 0.3f;
        } else continue;
        if (pos[1] < e->pos[1] - 1.0f || pos[1] > e->pos[1] + (e->dim[2] > 0 ? e->dim[2] : 3.0f)) continue;
        float c = cosf(e->yaw), s = sinf(e->yaw);
        float dx = pos[0] - e->pos[0], dz = pos[2] - e->pos[2];
        float lx = c * dx - s * dz - cx, lz = s * dx + c * dz - cz;   /* inverse of ent_local */
        float px = hx + r - fabsf(lx), pz = hz + r - fabsf(lz);
        if (px <= 0 || pz <= 0) continue;
        if (px < pz) lx += lx < 0 ? -px : px; else lz += lz < 0 ? -pz : pz;
        lx += cx; lz += cz;
        pos[0] = e->pos[0] + c * lx + s * lz;
        pos[2] = e->pos[2] - s * lx + c * lz;
        hit = 1;
    }
    return hit;
}

/* save: "AEVS", u32 version, respawn, n_flags, n_items, flags[n], items[n] (key, count) */
int ev_save_size(const Ev *ev)
{
    int n = 0;
    for (int i = 0; i < ev->n_flags; i++) n += !is_volatile(ev->flags[i]);
    return 20 + n * 4 + ev->n_items * 8;
}

int ev_save_write(const Ev *ev, void *buf, int cap)
{
    int need = ev_save_size(ev);
    if (cap < need) return -1;
    uint32_t *w = (uint32_t *)buf;
    int nf = 0;
    memcpy(w, "AEVS", 4);
    w[1] = 1; w[2] = ev->respawn; w[4] = (uint32_t)ev->n_items;
    uint32_t *f = w + 5;
    for (int i = 0; i < ev->n_flags; i++) if (!is_volatile(ev->flags[i])) f[nf++] = ev->flags[i];
    w[3] = (uint32_t)nf;
    uint32_t *it = f + nf;
    for (int i = 0; i < ev->n_items; i++) { it[2 * i] = ev->items[i].key; it[2 * i + 1] = ev->items[i].count; }
    return need;
}

int ev_save_read(Ev *ev, const void *buf, int len)
{
    const uint32_t *w = (const uint32_t *)buf;
    if (len < 20 || memcmp(w, "AEVS", 4) || w[1] != 1) return -1;
    uint32_t nf = w[3], ni = w[4];
    if (nf > EV_MAX_FLAGS || ni > EV_MAX_ITEMS || (int)(20 + nf * 4 + ni * 8) > len) return -2;
    ev->respawn = w[2];
    ev->n_flags = 0;
    for (uint32_t i = 0; i < nf; i++) ev_set_flag(ev, w[5 + i], 1);
    const uint32_t *it = w + 5 + nf;
    ev->n_items = (int)ni;
    for (uint32_t i = 0; i < ni; i++) { ev->items[i].key = it[2 * i]; ev->items[i].count = (uint16_t)it[2 * i + 1]; }
    return 0;
}
