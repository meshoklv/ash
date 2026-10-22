/* combat.ashcb loader: validates the header and section record sizes, then points into the buffer. */
#include "cbdata.h"

#include <string.h>

typedef char cb_assert_kind[sizeof(CbKind) == 84 ? 1 : -1];
typedef char cb_assert_anim[sizeof(CbAnim) == 24 ? 1 : -1];
typedef char cb_assert_state[sizeof(CbState) == 8 ? 1 : -1];
typedef char cb_assert_hit[sizeof(CbHit) == 16 ? 1 : -1];
typedef char cb_assert_evt[sizeof(CbEvt) == 12 ? 1 : -1];
typedef char cb_assert_atk[sizeof(CbAtk) == 88 ? 1 : -1];
typedef char cb_assert_dummy[sizeof(CbDummy) == 48 ? 1 : -1];
typedef char cb_assert_hurt[sizeof(CbHurt) == 68 ? 1 : -1];
typedef char cb_assert_npc[sizeof(CbNpc) == 64 ? 1 : -1];
typedef char cb_assert_weapon[sizeof(CbWeapon) == 64 ? 1 : -1];
typedef char cb_assert_throw[sizeof(CbThrow) == 48 ? 1 : -1];
typedef char cb_assert_player[sizeof(CbPlayer) == 64 ? 1 : -1];

static const int rec_size[CB_S_COUNT] = {84, 24, 8, 16, 12, 88, 48, 68, 64, 64, 48, 64};

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

int cb_data_load(CbData *d, const void *file, int size)
{
    const uint8_t *f = (const uint8_t *)file;
    const void *sec[CB_S_COUNT];
    int cnt[CB_S_COUNT], i;
    memset(d, 0, sizeof *d);
    if (size < 16 || memcmp(f, "ACMB", 4) != 0) return -1;
    if (rd32(f + 4) != CB_VERSION) return -2;
    if ((int)rd32(f + 8) < CB_S_COUNT || size < 16 + 8 * CB_S_COUNT) return -3;
    if (((uintptr_t)f & 3) != 0) return -4;
    for (i = 0; i < CB_S_COUNT; i++) {
        uint32_t off = rd32(f + 16 + 8 * i), n = rd32(f + 20 + 8 * i);
        if (off > (uint32_t)size || (uint64_t)off + (uint64_t)n * rec_size[i] > (uint64_t)size) return -5;
        sec[i] = f + off;
        cnt[i] = (int)n;
    }
    d->file = f; d->size = size;
    d->kind = sec[CB_S_KIND];     d->n_kind = cnt[CB_S_KIND];
    d->anim = sec[CB_S_ANIM];     d->n_anim = cnt[CB_S_ANIM];
    d->state = sec[CB_S_STATE];   d->n_state = cnt[CB_S_STATE];
    d->hit = sec[CB_S_HIT];       d->n_hit = cnt[CB_S_HIT];
    d->evt = sec[CB_S_EVT];       d->n_evt = cnt[CB_S_EVT];
    d->atk = sec[CB_S_ATK];       d->n_atk = cnt[CB_S_ATK];
    d->dummy = sec[CB_S_DUMMY];   d->n_dummy = cnt[CB_S_DUMMY];
    d->hurt = sec[CB_S_HURT];     d->n_hurt = cnt[CB_S_HURT];
    d->npc = sec[CB_S_NPC];       d->n_npc = cnt[CB_S_NPC];
    d->weapon = sec[CB_S_WEAPON]; d->n_weapon = cnt[CB_S_WEAPON];
    d->throws = sec[CB_S_THROW];  d->n_throw = cnt[CB_S_THROW];
    d->player = cnt[CB_S_PLAYER] ? sec[CB_S_PLAYER] : 0;
    for (i = 0; i < d->n_kind; i++) {
        const CbKind *k = &d->kind[i];
        if (k->anim_first + k->n_anim > d->n_anim || k->dummy_first + k->n_dummy > d->n_dummy ||
            k->hurt_first + k->n_hurt > d->n_hurt) return -6;
    }
    for (i = 0; i < d->n_anim; i++) {
        const CbAnim *a = &d->anim[i];
        if (a->state_first + a->n_state > d->n_state || a->hit_first + a->n_hit > d->n_hit ||
            a->evt_first + a->n_evt > d->n_evt) return -7;
    }
    return 0;
}

int cb_kind_find(const CbData *d, const char *name)
{
    int i;
    for (i = 0; i < d->n_kind; i++)
        if (strncmp(d->kind[i].name, name, 8) == 0) return i;
    return -1;
}

const CbAnim *cb_anim_find(const CbData *d, int kind, uint32_t anim_id)
{
    const CbAnim *a;
    int lo, hi;
    if (kind < 0 || kind >= d->n_kind) return 0;
    a = d->anim + d->kind[kind].anim_first;
    lo = 0; hi = d->kind[kind].n_anim - 1;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        if (a[mid].anim_id == anim_id) return &a[mid];
        if (a[mid].anim_id < anim_id) lo = mid + 1; else hi = mid - 1;
    }
    return 0;
}

const CbNpc *cb_npc_find(const CbData *d, uint32_t id)
{
    int i;
    for (i = 0; i < d->n_npc; i++) if (d->npc[i].id == id) return &d->npc[i];
    return 0;
}

const CbWeapon *cb_weapon_find(const CbData *d, uint32_t id)
{
    int i;
    for (i = 0; i < d->n_weapon; i++) if (d->weapon[i].id == id) return &d->weapon[i];
    return 0;
}

const CbThrow *cb_throw_find(const CbData *d, int atk_chr, int def_chr, int riposte)
{
    int i;
    /* ThrowType 8 = parry riposte (パリィ), 0 = backstab (背面) in the rows we cook; reinforced (強化, 11/12) skipped */
    for (i = 0; i < d->n_throw; i++) {
        const CbThrow *t = &d->throws[i];
        if (t->atk_chr == atk_chr && t->def_chr == def_chr && t->throw_type == (riposte ? 8 : 0)) return t;
    }
    return 0;
}
