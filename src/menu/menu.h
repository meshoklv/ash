/*
 * Ash menus in the DS1 style (docs/menu.md): title "НАЖМИТЕ ЛЮБУЮ КНОПКУ" -> main menu (Продолжить / Новая игра /
 * Настройки / Выйти из игры) -> character creation (name with our own on-screen keyboard, gender, all 10 classes of
 * CharaInitParam 3000..3009 with stats and starting gear, gift, 4 hair presets, rotating 3D preview of the class in
 * its starting armour) -> in-game menus (pause bar, inventory, equipment, status, settings, bonfire, level up,
 * "ВЫ ПОГИБЛИ"). Text and art from the DSR copy: tools/cook_menu.py (menu.ashui, menu.ashcls) and
 * tools/cook_classes.py (cls_<row>_<m|f>.ashc, head_<m|f>_<k>.ashc, menu_idle.ashanim).
 *
 * Uses src/ui (ui_text / ui_rect ... read-only API) and src/game chr.c / anim.c (preview). Everything is driven from
 * outside, one call per fixed step + one draw per frame:
 *
 *   Menu m;  menu_init(&m);                       // loads menu.ashui + menu.ashcls (ash_load_file search paths)
 *   menu_set_has_save(&m, save_exists);
 *   menu_open(&m, MENU_TITLE);
 *   loop: int ev = menu_tick(&m, pad.Buttons, dt);   // raw PSP_CTRL_* mask (held); edges are found inside
 *         switch (ev) { case MEV_NEW_GAME: m.chr has the created character ... }
 *         [3D world unless menu_fullscreen(&m)] ... menu_draw(&m);   // inside the frame's display list, after 3D
 *
 * In-game screens read MenuGameView (the game fills it before menu_open / every frame; stub data is fine).
 * Colours are GE ABGR. Strings are UTF-8 (FMG RUSSIAN). Nothing here allocates per frame.
 */
#ifndef ASH_MENU_H
#define ASH_MENU_H

#include <psptypes.h>
#include <pspgu.h>

#include "ui.h"
#include "menustr.h"
#include "chr.h"
#include "anim.h"

/* ------------------------------------------------------------------ cooked tables (menu.ashcls, little-endian) */

enum { MEQ_R1, MEQ_R2, MEQ_L1, MEQ_L2, MEQ_HEAD, MEQ_BODY, MEQ_ARMS, MEQ_LEGS, MEQ_N };
enum { MA_VIT, MA_ATT, MA_END, MA_STR, MA_DEX, MA_RES, MA_INT, MA_FTH, MA_N };

typedef struct { char magic[4]; u32 version; u16 n_class, n_gift, n_hair, pad; } MenuClsHdr;
typedef struct {
    u16 row, level;
    u8 attr[MA_N];
    u8 humanity, bare_head, n_goods, pad;
    u16 s_name, s_desc;
    u16 eq_str[MEQ_N], eq_img[MEQ_N];      /* 0xffff = empty */
    u16 goods_str[2], goods_img[2], goods_cnt[2], goods_desc[2];
    float weight;
} MenuClassRec;                            /* 72 B */
typedef struct { u16 s_name, s_desc, item_str, item_img, count, item_desc; } MenuGiftRec;   /* 12 B */
typedef struct { u16 s_name, s_desc; u8 gender, index; u16 pad; } MenuHairRec;              /* 8 B */

#define MENU_NONE16 0xffff
/* MenuItem.img >= MENU_IMG_EXT: image (img - MENU_IMG_EXT) of the extra resource given by menu_set_ext_images()
   (src/items items.ashui: item icons). */
#define MENU_IMG_EXT 0x4000

/* ------------------------------------------------------------------ what the menu gives / takes */

typedef struct {
    char name[64];          /* UTF-8, <= MENU_NAME_MAX code points */
    int gender;             /* 0 male, 1 female */
    int class_idx;          /* 0..9; CharaInitParam row = 3000 + class_idx */
    int class_row;
    int gift_idx;           /* 0..8; CharaInitParam row = 2400 + gift_idx (0 = no gift) */
    int hair;               /* 0..3 preset (tools/cook_classes.py HAIR_PRESETS) */
    int level, humanity;
    u8 attr[MA_N];
} MenuChar;

typedef struct {
    int scheme;             /* 0 = A, 1 = B (src/game/input.h InputState.scheme) */
    int brightness;         /* 0..10, 5 = neutral (menu_draw_brightness) */
    int cam_speed;          /* 1..10, 5 = default */
    int invert_x, invert_y;
    int overlay;            /* PSP-est overlay on/off */
} MenuSettings;

typedef struct {
    const char *name;       /* UTF-8 or 0 = empty slot */
    const char *desc;       /* may be 0 */
    int img;                /* image index in menu.ashui (MenuClassRec.eq_img ...) or -1 */
    int count;              /* 0 = do not show */
    float weight;
} MenuItem;

enum {                      /* equipment screen slots, DS1 order */
    MSLOT_R1, MSLOT_R2, MSLOT_L1, MSLOT_L2, MSLOT_ARROW1, MSLOT_ARROW2, MSLOT_BOLT1, MSLOT_BOLT2,
    MSLOT_HEAD, MSLOT_BODY, MSLOT_ARMS, MSLOT_LEGS, MSLOT_RING1, MSLOT_RING2,
    MSLOT_Q1, MSLOT_Q2, MSLOT_Q3, MSLOT_Q4, MSLOT_Q5, MSLOT_COUNT
};

#define MENU_MAX_ITEMS 32

typedef struct {
    const char *name, *class_name;
    int level, souls, humanity;
    u8 attr[MA_N];
    float hp, hp_max, sp, sp_max, load, load_max, poise, phys_def;
    int atk_r1, atk_l1;
    MenuItem slot[MSLOT_COUNT];
    MenuItem item[MENU_MAX_ITEMS];
    int n_items;
} MenuGameView;

enum {
    MENU_NONE,
    MENU_TITLE,             /* "НАЖМИТЕ ЛЮБУЮ КНОПКУ" -> MENU_MAIN */
    MENU_MAIN,
    MENU_CREATE,
    MENU_PAUSE,             /* START in game: icon bar */
    MENU_INVENTORY,
    MENU_EQUIP,
    MENU_STATUS,
    MENU_SETTINGS,
    MENU_BONFIRE,
    MENU_LEVELUP,
    MENU_DIED,
    MENU_SCREEN_COUNT
};

enum {                      /* menu_tick results */
    MEV_NONE,
    MEV_NEW_GAME,           /* m->chr = created character, m->set = settings */
    MEV_CONTINUE,           /* load the save */
    MEV_QUIT,               /* "Выйти из игры" confirmed -> sceKernelExitGame */
    MEV_CLOSE,              /* in-game menu closed -> back to gameplay */
    MEV_REST,               /* bonfire: rest (refill Estus, respawn enemies, save) - menu stays open */
    MEV_LEAVE_BONFIRE,      /* bonfire menu closed: stand up */
    MEV_LEVELUP,            /* level-up confirmed: m->view->attr / level / souls already updated */
    MEV_TO_TITLE,           /* "В главное меню" confirmed -> save, unload zone, menu_open(MENU_TITLE) */
    MEV_SETTINGS,           /* a setting changed (m->set) - apply at once (scheme, brightness ...) */
    MEV_USE_ITEM,           /* inventory "use": m->ev_arg = index in view->item */
    MEV_DIED_DONE,          /* "ВЫ ПОГИБЛИ" finished (fade to black) -> reload at the bonfire */
};

#define MENU_NAME_MAX 16
#define MENU_STACK 6

typedef struct {
    int screen;
    int cur;                /* cursor */
    int sub;                /* sub-state (open list, dialog ...) */
    int sub_cur;
    int scroll;
    float t;                /* time on this screen */
} MenuPage;

typedef struct {
    /* resources */
    UiRes ui;
    u8 *ui_file, *cls_file;
    int ui_size, cls_size;
    const MenuClsHdr *ch;
    const MenuClassRec *cls;
    const MenuGiftRec *gift;
    const MenuHairRec *hair;
    /* preview (character creation) */
    Chr body, head;
    u8 *body_file, *head_file;
    int body_size, head_size;
    int body_key, head_key;  /* loaded class*2+gender / gender*4+hair, -1 none */
    AnimSet idle;
    u8 *idle_file;
    int idle_size, idle_clip;
    float prev_t, prev_yaw;
    int load_count;          /* model loads (for the log) */
    BonePose __attribute__((aligned(16))) pose[ANIM_MAX_BONES];
    ScePspFMatrix4 __attribute__((aligned(16))) world[ANIM_MAX_BONES], skin[ANIM_MAX_BONES];
    /* scene light the game uses (restored after the preview) */
    float light_dir[3];
    u32 light_col, ambient;
    /* state */
    MenuPage stack[MENU_STACK];
    int depth;
    u32 held, pressed, prev;
    float rep_t;             /* d-pad auto-repeat */
    u32 rep_btn;
    int has_save;
    int ev_arg;
    float died_t;
    MenuChar chr;
    MenuSettings set;
    MenuGameView *view;
    MenuGameView own_view;   /* used when the game gives none */
    const UiRes *ext_ui;     /* extra images (MENU_IMG_EXT), 0 = none */
    /* on-screen keyboard */
    int osk_on, osk_cur, osk_latin, osk_lower;
    char osk_buf[64];
    /* level up working copy */
    u8 lv_attr[MA_N];
    int lv_level, lv_souls;
    int dbg_frames;
} Menu;

int menu_init(Menu *m);                      /* 0 ok; <0 file missing (log says which) */
void menu_free(Menu *m);
void menu_set_has_save(Menu *m, int has);
void menu_set_view(Menu *m, MenuGameView *v);   /* 0 -> built-in stub from m->chr */
void menu_set_ext_images(Menu *m, const UiRes *r);   /* src/items: item icons for MenuItem.img >= MENU_IMG_EXT */
void menu_set_scene_light(Menu *m, const float dir[3], u32 diffuse, u32 ambient);
void menu_open(Menu *m, int screen);         /* replaces the whole stack */
void menu_close(Menu *m);
int menu_screen(const Menu *m);              /* top screen or MENU_NONE */
int menu_fullscreen(const Menu *m);          /* title / main / creation: the game draws no world */
int menu_blocks_game(const Menu *m);         /* any screen that eats the pad (all except MENU_DIED) */
int menu_tick(Menu *m, u32 buttons, float dt);
void menu_draw(Menu *m);                     /* inside the display list, after the 3D pass */
/* Full-screen brightness from MenuSettings (call after the 3D pass, before HUD) [придумал: 0..4 darken, 6..10 add]. */
void menu_draw_brightness(const MenuSettings *s);
/* Starting stats of the created character -> view (name, class, level, attributes, equipment names/icons, items). */
void menu_fill_view_from_char(const Menu *m, MenuGameView *v);
u32 menu_mem_bytes(const Menu *m);           /* files + preview models currently loaded */
const char *menu_dstr(const Menu *m, int id);   /* string by index (fixed MS_* or dynamic), "" for 0xffff */

#endif
