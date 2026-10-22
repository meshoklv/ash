/*
 * Bridge src/items -> src/menu: fills MenuGameView (equipment slots, inventory list, HP / stamina / load / poise /
 * defence / attack) from the real inventory instead of menu_fill_view_from_char's stub. PSP build only (menu.h).
 * Item icons: items.ashui loaded with ui_load() and given to the menu with menu_set_ext_images() (needs
 * src/items/menu-ext-images.patch in src/menu); MenuItem.img = MENU_IMG_EXT + ItmRec.icon. Without the patch the
 * menu simply shows no icon for those items (ui_has_img fails on the out-of-range id).
 */
#ifndef ASH_ITEMS_MENU_H
#define ASH_ITEMS_MENU_H

#include "items.h"
#include "menu.h"

#ifndef MENU_IMG_EXT
#define MENU_IMG_EXT 0x4000
#endif

typedef struct {
    int16_t inv_idx[MENU_MAX_ITEMS];   /* view->item[k] -> inventory index (MEV_USE_ITEM: m->ev_arg -> inv_idx) */
    int n;
} ItmMenuMap;

/* Fills v->slot[], v->item[] (estus, consumables, keys, then weapons, shields, armour, rings; at most
   MENU_MAX_ITEMS), v->hp_max / sp_max / load / load_max / poise / phys_def / atk_r1 / atk_l1 from st.
   Leaves name, class, level, souls, humanity, attributes and current hp / sp to the caller (clamps hp/sp to max). */
void itm_menu_fill(const ItmDb *db, const Inv *inv, const ItmStats *st, MenuGameView *v, ItmMenuMap *map);

#endif
