/* src/items -> src/menu bridge (items_menu.h). */
#include <string.h>

#include "items_menu.h"

static void put(const ItmDb *db, const Inv *inv, int idx, MenuItem *it)
{
    const ItmRec *r = itm_find(db, inv->e[idx].key);
    memset(it, 0, sizeof(*it));
    it->img = -1;
    if (!r) return;
    it->name = itm_str(db, r->name);
    it->desc = itm_str(db, r->desc);
    it->img = r->icon != 0xffff ? MENU_IMG_EXT + r->icon : -1;
    it->weight = r->weight;
    if (r->cat == ITM_CAT_GOODS) it->count = r->kind == ITM_GK_ESTUS ? (inv->estus ? (int)*inv->estus : 1) : inv->e[idx].count;
    else if (r->cat == ITM_CAT_WEAPON && (r->kind == ITM_WK_ARROW || r->kind == ITM_WK_BOLT)) it->count = inv->e[idx].count;
}

/* list order (DS1 inventory tabs collapsed into one list) [придумал] */
static int group(const ItmRec *r)
{
    if (!r) return 9;
    if (r->cat == ITM_CAT_GOODS) return r->kind == ITM_GK_ESTUS ? 0 : r->kind == ITM_GK_KEY ? 2 : 1;
    if (r->cat == ITM_CAT_WEAPON) return r->kind == ITM_WK_SHIELD ? 4 : (r->kind == ITM_WK_ARROW || r->kind == ITM_WK_BOLT) ? 5 : 3;
    if (r->cat == ITM_CAT_ARMOR) return 6;
    return 7;
}

void itm_menu_fill(const ItmDb *db, const Inv *inv, const ItmStats *st, MenuGameView *v, ItmMenuMap *map)
{
    for (int s = 0; s < ITM_SLOT_COUNT && s < MSLOT_COUNT; s++) {
        int idx = itm_equipped(inv, s);
        if (idx >= 0) put(db, inv, idx, &v->slot[s]);
        else { memset(&v->slot[s], 0, sizeof(v->slot[s])); v->slot[s].img = -1; }
    }
    v->n_items = 0;
    if (map) map->n = 0;
    for (int g = 0; g <= 9 && v->n_items < MENU_MAX_ITEMS; g++)
        for (int i = 0; i < inv->n && v->n_items < MENU_MAX_ITEMS; i++) {
            if (group(itm_find(db, inv->e[i].key)) != g) continue;
            if (map) map->inv_idx[v->n_items] = (int16_t)i;
            put(db, inv, i, &v->item[v->n_items++]);
        }
    if (map) map->n = v->n_items;
    if (st) {
        v->hp_max = st->hp_max;
        v->sp_max = st->sp_max;
        if (v->hp > v->hp_max || v->hp <= 0) v->hp = v->hp_max;
        if (v->sp > v->sp_max || v->sp <= 0) v->sp = v->sp_max;
        v->load = st->load;
        v->load_max = st->load_max;
        v->poise = st->poise;
        v->phys_def = st->def[0];
        v->atk_r1 = (int)(st->ar_r[0] + st->ar_r[1] + st->ar_r[2] + st->ar_r[3] + 0.5f);
        v->atk_l1 = (int)(st->ar_l[0] + st->ar_l[1] + st->ar_l[2] + st->ar_l[3] + 0.5f);
    }
}
