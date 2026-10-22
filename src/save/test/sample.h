/* A deterministic, fully populated SaveGame for the save tests (native and EBOOT). variant changes souls etc. */
#ifndef ASH_SAVE_SAMPLE_H
#define ASH_SAVE_SAMPLE_H

#include <string.h>
#include "savegame.h"

static void sample_game(SaveGame *s, int variant)
{
    sg_init(s);
    s->play_s = 3725 + variant;
    s->area = 18; s->block = 1;
    s->bonfire = 1811960;            /* sample entity id */
    s->flags = 0x5u + (uint32_t)variant;
    strcpy(s->name, "Пепел");        /* UTF-8, 5 code points */
    s->class_row = 3000;             /* Warrior */
    s->gender = 1; s->gift = 2; s->hair = 3;
    s->level = 4 + variant;
    static const uint8_t attr[8] = {11, 8, 12, 13, 13, 11, 9, 9};
    memcpy(s->attr, attr, 8);
    s->attr[3] += variant;
    s->humanity = 2; s->souls = 1234 + 1000 * variant;
    s->hp = 512.5f; s->hp_max = 572.0f;
    s->estus = 3; s->estus_max = 5; s->deaths = 7;
    s->prg_flags = 0xA5;
    s->stain_on = 1; s->stain_area = 18; s->stain_block = 1;
    s->stain_pos[0] = 3.25f; s->stain_pos[1] = -1.5f; s->stain_pos[2] = 140.125f;
    s->stain_souls = 60;
    s->n_item = 6;
    s->item[0] = (SgItem){SG_CAT_WEAPON, 0, 1, 201000};    /* sample ids */
    s->item[1] = (SgItem){SG_CAT_WEAPON, 2, 1, 1450000};
    s->item[2] = (SgItem){SG_CAT_ARMOR, 0, 1, 10000};
    s->item[3] = (SgItem){SG_CAT_RING, 0, 1, 100};
    s->item[4] = (SgItem){SG_CAT_GOODS, 0, 5, 200};
    s->item[5] = (SgItem){SG_CAT_GOODS, 0, 3, 2001};
    s->equip[0] = 0; s->equip[2] = 1; s->equip[9] = 2; s->equip[12] = 3; s->equip[14] = 4;
    s->scheme = 1; s->brightness = 7; s->cam_speed = 4; s->invert_x = 0; s->invert_y = 1; s->overlay = 1;
    SgZoneEvents *z = sg_zone_put(s, 18, 1);
    z->size = 1500 + variant * 4;
    for (int i = 0; i < z->size; i++) z->data[i] = (uint8_t)(i * 7 + 3 + variant);
    s->itm_size = 576;   /* stands in for itm_save_write */
    for (int i = 0; i < s->itm_size; i++) s->itm_blob[i] = (uint8_t)(i * 13 + variant);
    z = sg_zone_put(s, 10, 2);
    z->size = 37;
    for (int i = 0; i < z->size; i++) z->data[i] = (uint8_t)(255 - i);
}

#endif
