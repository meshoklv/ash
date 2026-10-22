#!/usr/bin/env python3
"""Cook the resources of src/menu (title, main menu, character creation, in-game menus) into two files:

    tools/.venv/bin/python tools/cook_menu.py --out-dir ~/ash-assets/cooked/menu [--lang RUSSIAN] [--png DIR]
    tools/.venv/bin/python tools/cook_menu.py --gen-header src/menu/menustr.h       # C enums (in git)

  menu.ashui   same container as src/ui (`.ashui` v1, tools/cook_ui.py writer, read by ui_load): font atlas,
               UTF-8 strings, CLUT8 images. Fonts [придумал: подобрано по глазу на 480x272]:
                 0 = DejaVu Serif 12 px (small text, descriptions; Bitstream Vera licence),
                 1 = EB Garamond 15 px (menu items), 2 = EB Garamond 22 px (titles) - EB Garamond is SIL OFL 1.1
                 (github.com/google/fonts ofl/ebgaramond, kept in ~/ash-assets/fonts with its OFL.txt).
               Strings: fixed ones (enum MS_* in src/menu/menustr.h) + dynamic ones (item names / descriptions)
               after MS_COUNT, referenced by index from menu.ashcls.
               Images: enum MI_* (title background, panel leather, gold lines, cursor bar, pause-menu icons,
               slot icons, "ВЫ ПОГИБЛИ") + dynamic item icons after MI_COUNT.
  menu.ashcls  class / gift / hair tables (little-endian, layout = src/menu/menu.h MenuClsHdr/MenuClassRec/...):
               Header 16 B: "ASCL", u32 version=1, u16 n_class, u16 n_gift, u16 n_hair, u16 pad
               Class 72 B : u16 row, u16 level, u8 attr[8] (VIT ATT END STR DEX RES INT FTH), u8 humanity,
                            u8 bare_head, u8 n_goods, u8 pad, u16 s_name, u16 s_desc,
                            u16 eq_str[8], u16 eq_img[8]  (R1 R2 L1 L2 head body arms legs; 0xffff = empty),
                            u16 goods_str[2], u16 goods_img[2], u16 goods_cnt[2], u16 goods_desc[2], f32 weight
               Gift 12 B  : u16 s_name, u16 s_desc, u16 item_str, u16 item_img, u16 count, u16 item_desc
               Hair 8 B   : u16 s_name, u16 s_desc, u8 gender (0 m, 1 f), u8 index, u16 pad
Sources: GameParam CharaInitParam 3000..3009 (classes), 2400..2408 (gifts), EquipParamProtector/Weapon/Goods/
Accessory (names, weights, icons); FMG menu 70/76/77/78/79/80 + item 10..13, 24, 27 (RUSSIAN); menu textures:
menu_4 clearscreen_bg (title), bk Menu01 (leather), Menu11 (pause icons), Menu12 (slot icons), Menu13 (gold lines,
cursor bar), RUSSIAN/menu_local Menu14 ("ВЫ ПОГИБЛИ"), Icon00..Icon50 (item icons, 12 columns, 144 per atlas).
Outputs hold FromSoftware text and art -> never in git.
"""
from __future__ import annotations

import argparse
import io
import json
import logging
import os
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
logging.disable(logging.WARNING)
import numpy as np  # noqa: E402
from PIL import Image, ImageDraw, ImageFont  # noqa: E402

import cook_ui as cu  # noqa: E402

DSR = Path(os.environ.get("ASH_DSR", Path.home() / "ash-assets/dsr"))
FONTS = [("/usr/share/fonts/truetype/dejavu/DejaVuSerif.ttf", 12, None),
         (str(Path.home() / "ash-assets/fonts/EBGaramond.ttf"), 15, 500),
         (str(Path.home() / "ash-assets/fonts/EBGaramond.ttf"), 22, 500)]
EXTRA = "«»—–…№°×·’“”○△□✕◀▶▲▼←→↑↓"

CLASS_ROWS = list(range(3000, 3010))
GIFT_ROWS = list(range(2400, 2409))
HAIR = {0: [1000, 1200, 1100, 1900], 1: [3100, 3300, 3500, 3900]}   # = tools/cook_classes.py HAIR_PRESETS

# (enum, source) - ("menu"|"item", fmg, id) or ("OWN", "text") [перевод свой]
STRINGS = [
    ("MS_PRESS_ANY", ("menu", 77, 130008)),       # НАЖМИТЕ ЛЮБУЮ КНОПКУ
    ("MS_CONTINUE", ("menu", 77, 401301)),        # Продолжить
    ("MS_NEW_GAME", ("menu", 77, 401303)),        # Новая игра
    ("MS_SETTINGS", ("menu", 70, 110005)),        # Настройки
    ("MS_QUIT", ("menu", 77, 401309)),            # Выйти из игры
    ("MS_QUIT_ASK", ("OWN", "Выйти из игры?")),
    ("MS_CREATE", ("menu", 77, 130021)),          # Создать персонажа
    ("MS_NAME", ("menu", 76, 101000)),            # Имя
    ("MS_GENDER", ("menu", 76, 101001)),          # Пол
    ("MS_CLASS", ("menu", 76, 101017)),           # Класс
    ("MS_GIFT", ("menu", 76, 101014)),            # Дар
    ("MS_HAIR", ("menu", 77, 133102)),            # Волосы
    ("MS_MALE", ("menu", 77, 132010)),
    ("MS_FEMALE", ("menu", 77, 132011)),
    ("MS_MALE_DESC", ("menu", 77, 132310)),
    ("MS_FEMALE_DESC", ("menu", 77, 132311)),
    ("MS_ENTER_NAME", ("menu", 77, 132100)),      # Введите имя персонажа
    ("MS_START_ASK", ("menu", 78, 42003)),        # Начать игру с этим персонажем?
    ("MS_DONE", ("OWN", "Готово")),
    ("MS_YES", ("OWN", "Да")),
    ("MS_NO", ("menu", 76, 10000)),               # Нет
    ("MS_OK", ("menu", 92, 1)),                   # OK
    ("MS_CANCEL", ("OWN", "Отмена")),
    ("MS_DEFAULT_NAME", ("OWN", "Нежить")),
    ("MS_HAIR_BALD_F", ("OWN", "Короткие")),
    ("MS_HELM_ON", ("OWN", "Голова скрыта шлемом")),
    ("MS_OSK_SPACE", ("OWN", "Пробел")),
    ("MS_OSK_DEL", ("OWN", "Стереть")),
    ("MS_OSK_ABC", ("OWN", "ABC")),
    ("MS_OSK_RUS", ("OWN", "АБВ")),
    # status labels
    ("MS_LEVEL", ("menu", 76, 101013)),           # Уровень
    ("MS_SOULS", ("menu", 76, 101041)),           # Души
    ("MS_VIT", ("menu", 76, 101003)),             # Здоровье (Vitality)
    ("MS_ATT", ("menu", 76, 101004)),             # Ученость
    ("MS_END", ("menu", 76, 101005)),             # Выносливость
    ("MS_STR", ("menu", 76, 101006)),             # Сила
    ("MS_DEX", ("menu", 76, 101007)),             # Ловкость
    ("MS_RES", ("menu", 76, 101008)),             # Сопротив.
    ("MS_INT", ("menu", 76, 101009)),             # Интеллект
    ("MS_FTH", ("menu", 76, 101010)),             # Вера
    ("MS_HUMANITY", ("menu", 76, 101012)),        # Человечность
    ("MS_HP", ("menu", 76, 101030)),              # Здоровье (HP)
    ("MS_STAMINA", ("OWN", "Энергия")),           # FMG has no separate label in the status list [версия]
    ("MS_LOAD", ("menu", 76, 101040)),            # Загруженность
    ("MS_POISE", ("menu", 76, 101053)),           # Баланс
    ("MS_PHYS_DEF", ("menu", 76, 101046)),        # Физ. защита
    ("MS_ATK_R1", ("menu", 76, 101042)),          # Пр. оружие 1
    ("MS_ATK_L1", ("menu", 76, 101044)),          # Лев. оружие 1
    ("MS_HAVE", ("menu", 76, 105000)),            # Имеется
    ("MS_NEED", ("menu", 76, 105001)),            # Треб. душ
    ("MS_WEIGHT", ("menu", 76, 102000)),          # Вес
    ("MS_COVENANT", ("menu", 76, 101002)),        # Орден
    ("MS_NONE", ("menu", 76, 105100)),            # Нет
    # in-game menu
    ("MS_INVENTORY", ("menu", 70, 110000)),       # Инвентарь
    ("MS_EQUIPMENT", ("menu", 70, 110001)),       # Снаряжение
    ("MS_STATUS", ("menu", 70, 110003)),          # Статус
    ("MS_SYSTEM", ("menu", 70, 110006)),          # Система
    ("MS_TO_TITLE", ("menu", 80, 16508)),         # В главное меню
    ("MS_TO_TITLE_ASK", ("OWN", "Выйти в главное меню?")),
    ("MS_USE", ("menu", 70, 111013)),             # Использовать
    ("MS_RIGHT", ("menu", 70, 112011)),           # Правая рука
    ("MS_LEFT", ("menu", 70, 112012)),            # Левая рука
    ("MS_ARROW", ("menu", 70, 112013)),           # Стрела
    ("MS_BOLT", ("menu", 70, 112014)),            # Болт
    ("MS_HEAD", ("menu", 70, 112001)),            # Голова
    ("MS_BODY", ("menu", 70, 112002)),            # Туловище
    ("MS_ARMS", ("menu", 70, 112003)),            # Руки
    ("MS_LEGS", ("menu", 70, 112004)),            # Ноги
    ("MS_RING", ("menu", 70, 112020)),            # Кольцо
    ("MS_QUICK", ("menu", 70, 112030)),           # Используемый предмет
    ("MS_EMPTY_SLOT", ("OWN", "Пусто")),
    # bonfire, death
    ("MS_REST", ("menu", 30, 10010183)),          # Отдохнуть у костра
    ("MS_LEVELUP", ("menu", 30, 15000100)),       # Новый уровень
    ("MS_LEAVE", ("menu", 30, 15000005)),         # Выйти
    ("MS_LEVELUP_ASK", ("OWN", "Повысить уровень?")),
    ("MS_DIED", ("OWN", "ВЫ ПОГИБЛИ")),            # texture Menu14 in DSR; text = fallback
    # settings
    ("MS_CONTROLS", ("menu", 70, 114000)),        # Управление
    ("MS_SCHEME", ("OWN", "Схема управления")),
    ("MS_SCHEME_A", ("OWN", "A: × уклон, □ удар")),
    ("MS_SCHEME_B", ("OWN", "B: ○ уклон, R удар")),
    ("MS_BRIGHTNESS", ("menu", 70, 114103)),      # Яркость
    ("MS_BRIGHT_HINT1", ("menu", 70, 114110)),    # Настройте яркость так, чтобы
    ("MS_BRIGHT_HINT2", ("menu", 70, 114111)),    # видеть пламя, а не дракона.
    ("MS_CAM_SPEED", ("menu", 70, 114004)),       # Скорость камеры
    ("MS_CAM_X", ("menu", 70, 114002)),           # Ось X
    ("MS_CAM_Y", ("menu", 70, 114003)),           # Ось Y
    ("MS_NORMAL", ("menu", 70, 114404)),          # Норма
    ("MS_INVERT", ("menu", 70, 114405)),          # Инверсия
    ("MS_ON", ("menu", 70, 114400)),              # ВКЛ
    ("MS_OFF", ("menu", 70, 114401)),             # ВЫКЛ
    ("MS_OVERLAY", ("OWN", "Оверлей PSP-est")),
    ("MS_DEFAULTS", ("menu", 70, 114489)),        # Настройки по умолчанию
    # help line
    ("MS_HELP_SELECT", ("OWN", "× Выбрать   ○ Назад")),
    ("MS_HELP_CHANGE", ("OWN", "◀▶ Изменить   × Выбрать   ○ Назад")),
    ("MS_HELP_TURN", ("OWN", "▲▼ Выбрать   L/R Повернуть   × Ввод   ○ Назад")),
    ("MS_HELP_OSK", ("OWN", "× Ввести   □ Стереть   △ Регистр   START Готово")),
    ("MS_ESTUS", ("item", 10, 201)),              # Фляга с Эстусом
    ("MS_ESTUS_DESC", ("item", 24, 201)),
]

# (enum, kind, args) - kinds: "bg" (tpf, stem, w, h), "crop" (tpf, stem, box, w, h), "died"
TPF_BK = "menu/bk.tpf.dcx"
IMAGES = [
    ("MI_TITLE_BG", "bg", ("menu/menu_4.tpf.dcx", "clearscreen_bg", 480, 256)),
    ("MI_LEATHER", "crop", (TPF_BK, "Menu01", (800, 150, 1312, 662), 128, 128)),
    ("MI_GOLD_LINE", "crop", (TPF_BK, "Menu13", (1400, 424, 1694, 456), 256, 12)),
    ("MI_CURSOR", "crop", (TPF_BK, "Menu13", (1733, 322, 2032, 377), 128, 24)),
    ("MI_ORNAMENT", "crop", (TPF_BK, "Menu13", (1706, 420, 1936, 496), 96, 32)),
    ("MI_ARROW", "crop", (TPF_BK, "Menu13", (1976, 413, 2019, 489), 12, 22)),
    # pause-menu icons, Menu11 top row (1024x512): Снаряжение, Инвентарь, Статус, (Магия), Настройки, (Жесты)
    ("MI_ICON_EQUIP", "cell", (TPF_BK, "Menu11", 0)),
    ("MI_ICON_INV", "cell", (TPF_BK, "Menu11", 1)),
    ("MI_ICON_STATUS", "cell", (TPF_BK, "Menu11", 2)),
    ("MI_ICON_MAGIC", "cell", (TPF_BK, "Menu11", 3)),
    ("MI_ICON_SYSTEM", "cell", (TPF_BK, "Menu11", 4)),
    ("MI_ICON_FACE", "cell", (TPF_BK, "Menu11", 5)),
    # empty equipment slots, Menu12 second row (2048x512) [версия: which icon is which slot - по рисунку]
    ("MI_SLOT_WEAPON", "slot", (0,)),
    ("MI_SLOT_SHIELD", "slot", (1,)),
    ("MI_SLOT_RING", "slot", (2,)),
    ("MI_SLOT_HELM", "slot", (3,)),
    ("MI_SLOT_ARROW", "slot", (4,)),
    ("MI_SLOT_BOLT", "slot", (5,)),
    ("MI_SLOT_ITEM", "slot", (6,)),
    ("MI_SLOT_ARMOR", "slot1", (0,)),
    ("MI_DIED", "died", ()),
    ("MI_ICON_ESTUS", "icon", (2005,)),          # EquipParamGoods 201 GoodIcon 2005
]

_FMG = None
_PARAMS = None
_TPF: dict = {}


def fmg(src) -> str:
    global _FMG
    if _FMG is None:
        _FMG = cu.load_fmgs(LANG)
    t = _FMG.get((src[0], src[1]), {}).get(src[2])
    if t is None:
        raise SystemExit(f"FMG text missing: {src}")
    return t.replace("\r", "")


def params():
    global _PARAMS
    if _PARAMS is None:
        from soulstruct.darksouls1r.params.gameparambnd import GameParamBND
        g = GameParamBND.from_path(DSR / "param/GameParam/GameParam.parambnd.dcx")
        _PARAMS = {k.rsplit("\\", 1)[1]: v.rows for k, v in g.params.items()}
    return _PARAMS


def tpf(path: str, stem: str) -> Image.Image:
    key = (path, stem)
    if key not in _TPF:
        _TPF[key] = cu.tpf_image(DSR / path, stem)
    return _TPF[key]


# ------------------------------------------------------------------------------------------- font

def build_font(text: str) -> dict:
    chars = set(chr(c) for c in range(32, 127)) | set(chr(c) for c in range(0x410, 0x450)) | {"Ё", "ё"}
    chars |= set(EXTRA) | set(text)
    chars -= {"\n", "\r", "\t"}
    chars = sorted(chars, key=ord)
    glyphs, fonts = [], []
    for path, size, wght in FONTS:
        f = ImageFont.truetype(path, size)
        if wght:
            try:
                f.set_variation_by_axes([wght])
            except Exception:
                pass
        asc, desc = f.getmetrics()
        first = len(glyphs)
        fallback = ImageFont.truetype(FONTS[0][0], size)
        for ch in chars:
            ff = f
            # glyph missing in EB Garamond (arrows, button shapes) -> DejaVu at the same size
            if path != FONTS[0][0] and ch not in " " and f.getmask(ch).getbbox() is None:
                ff = fallback
            if ch == " ":
                glyphs.append(dict(cp=32, img=None, w=0, h=0, xoff=0, yoff=0, adv=round(f.getlength(" "))))
                continue
            bbox = ff.getbbox(ch, anchor="ls")
            if bbox[2] <= bbox[0] or bbox[3] <= bbox[1]:
                glyphs.append(dict(cp=ord(ch), img=None, w=0, h=0, xoff=0, yoff=0, adv=round(ff.getlength(ch))))
                continue
            im = Image.new("L", (bbox[2] - bbox[0], bbox[3] - bbox[1]), 0)
            ImageDraw.Draw(im).text((-bbox[0], -bbox[1]), ch, font=ff, fill=255, anchor="ls")
            glyphs.append(dict(cp=ord(ch), img=np.asarray(im), w=im.width, h=im.height, xoff=bbox[0], yoff=bbox[1],
                               adv=round(ff.getlength(ch))))
        fonts.append(dict(line_h=asc + desc, ascent=asc, first=first, n=len(glyphs) - first))
    W = 512
    order = sorted(range(len(glyphs)), key=lambda i: -glyphs[i]["h"])
    x = y = shelf = 0
    for i in order:
        g = glyphs[i]
        if g["img"] is None:
            g["x"] = g["y"] = 0
            continue
        if x + g["w"] + 1 > W:
            x, y, shelf = 0, y + shelf + 1, 0
        g["x"], g["y"] = x, y
        x += g["w"] + 1
        shelf = max(shelf, g["h"])
    H = 1
    while H < y + shelf + 1:
        H *= 2
    assert H <= 512, f"font atlas too tall: {H}"
    atlas = np.zeros((H, W), np.uint8)
    for g in glyphs:
        if g["img"] is not None:
            atlas[g["y"]:g["y"] + g["h"], g["x"]:g["x"] + g["w"]] = g["img"]
    a4 = ((atlas.astype(np.uint32) * 15 + 127) // 255).astype(np.uint8)
    packed = (a4[:, 0::2] | (a4[:, 1::2] << 4)).astype(np.uint8)
    return dict(W=W, H=H, pix=packed.tobytes(), glyphs=glyphs, fonts=fonts, atlas_preview=a4 * 17, chars=chars)


# ------------------------------------------------------------------------------------------- images

def icon_cell(atlas: Image.Image, k: int, size: int, cols: int = 12) -> Image.Image:
    """Cut icon k of a 12-column DSR icon atlas: the run of opaque columns nearest the cell centre (cook_ui.py)."""
    cell = atlas.width / cols
    r, c = divmod(k, cols)
    a = np.asarray(atlas)
    al = a[round(r * cell):round((r + 1) * cell), :, 3]
    occ = np.r_[False, al.max(axis=0) > 30, False]
    edges = np.flatnonzero(occ[1:] != occ[:-1])
    runs = list(zip(edges[0::2], edges[1::2]))
    if not runs:
        return Image.new("RGBA", (size, size))
    cx = (c + 0.5) * cell
    x0, x1 = min(runs, key=lambda q: abs((q[0] + q[1]) / 2 - cx))
    rows = np.flatnonzero(al[:, x0:x1].max(axis=1) > 30)
    side = max(x1 - x0, rows[-1] - rows[0]) + 8
    mx, my = (x0 + x1) / 2, r * cell + (rows[0] + rows[-1]) / 2
    crop = atlas.crop((round(mx - side / 2), round(my - side / 2), round(mx + side / 2), round(my + side / 2)))
    return crop.resize((size, size), Image.Resampling.LANCZOS)


ICON_TPF = {}


def item_icon(icon_id: int, size: int = 32) -> Image.Image | None:
    """Icon id (GoodIcon / WeaponIcon / MaleIcon / MenuIcon) -> IconXY atlas: X = id // 1000, Y = (id % 1000) // 144,
    index (id % 1000) % 144, 12 columns [проверено глазами на Icon20 в cook_ui.py; остальные атласы - по той же схеме]."""
    if icon_id is None or icon_id < 0:
        return None
    stem = f"Icon{icon_id // 1000}{(icon_id % 1000) // 144}"
    if stem not in ICON_TPF:
        ICON_TPF[stem] = None
        for f in ("menu/menu_0.tpf.dcx", "menu/menu_3.tpf.dcx"):
            try:
                ICON_TPF[stem] = cu.tpf_image(DSR / f, stem)
                break
            except StopIteration:
                continue
    at = ICON_TPF[stem]
    if at is None:
        return None
    return icon_cell(at, (icon_id % 1000) % 144, size)


def band_icons(im: Image.Image, y0: int, y1: int) -> list[tuple[int, int]]:
    """Column runs of opaque pixels inside a horizontal band -> [(x0, x1)], gaps < 6 px merged."""
    al = np.asarray(im)[y0:y1, :, 3]
    occ = np.r_[False, al.max(axis=0) > 40, False]
    e = np.flatnonzero(occ[1:] != occ[:-1])
    runs = [list(r) for r in zip(e[0::2], e[1::2])]
    out = []
    for r in runs:
        if out and r[0] - out[-1][1] < 6:
            out[-1][1] = r[1]
        else:
            out.append(r)
    return [(a, b) for a, b in out if b - a > 40]


def fit(im: Image.Image, w: int, h: int) -> Image.Image:
    return im.resize((w, h), Image.Resampling.LANCZOS)


def build_images() -> list:
    out = []
    for name, kind, args in IMAGES:
        if kind == "bg":
            path, stem, w, h = args
            im = tpf(path, stem)
            im = fit(im, w, h)
            a = np.asarray(im).copy()
            a[:, :, 3] = 255
            out.append(cu.to_t8(Image.fromarray(a, "RGBA")))
        elif kind == "crop":
            path, stem, box, w, h = args
            out.append(cu.to_t8(fit(tpf(path, stem).crop(box), w, h)))
        elif kind == "cell":
            path, stem, k = args
            im = tpf(path, stem)                    # 1024x512, six 150-px icons in the top row
            runs = band_icons(im, 0, 165)
            x0, x1 = runs[k]
            al = np.asarray(im)[0:170, x0:x1, 3]
            rows = np.flatnonzero(al.max(axis=1) > 40)
            out.append(cu.to_t8(fit(im.crop((x0, rows[0], x1, rows[-1] + 1)), 40, 40)))
        elif kind in ("slot", "slot1"):
            im = tpf(TPF_BK, "Menu12")               # 2048x512: row 1 y 10..160, row 2 y 176..320
            y0, y1 = (176, 330) if kind == "slot" else (10, 170)
            runs = band_icons(im, y0, y1)
            x0, x1 = runs[args[0]]
            al = np.asarray(im)[y0:y1, x0:x1, 3]
            rows = np.flatnonzero(al.max(axis=1) > 40)
            out.append(cu.to_t8(fit(im.crop((x0, y0 + rows[0], x1, y0 + rows[-1] + 1)), 32, 32)))
        elif kind == "icon":
            out.append(cu.to_t8(item_icon(args[0])))
        elif kind == "died":
            if LANG != "RUSSIAN":
                out.append(None)
                continue
            name_, stem, (x0, y0, x1, y1), tw = cu.BANNERS[0]
            band = cu.luma_key(tpf("menu/RUSSIAN/menu_local.tpf.dcx", stem).crop((x0, y0, x1, y1)))
            al = np.asarray(band)[:, :, 3]
            ys, xs = np.nonzero(al > 24)
            crop = band.crop((max(0, xs.min() - 4), max(0, ys.min() - 4), min(band.width, xs.max() + 5),
                              min(band.height, ys.max() + 5)))
            tw = 300
            out.append(cu.to_t8(crop.resize((tw, max(1, round(crop.height * tw / crop.width))),
                                            Image.Resampling.LANCZOS)))
    return out


# ------------------------------------------------------------------------------------------- tables

class Dyn:
    """Dynamic strings / images appended after the fixed enums."""

    def __init__(self, n_str: int, n_img: int):
        self.strs: list[str] = []
        self.sidx: dict[str, int] = {}
        self.imgs: list = []
        self.iidx: dict[int, int] = {}
        self.n_str, self.n_img = n_str, n_img

    def s(self, text: str | None) -> int:
        if not text:
            return 0xFFFF
        if text not in self.sidx:
            self.sidx[text] = self.n_str + len(self.strs)
            self.strs.append(text)
        return self.sidx[text]

    def icon(self, icon_id: int | None) -> int:
        if icon_id is None or icon_id <= 0:
            return 0xFFFF
        if icon_id not in self.iidx:
            im = item_icon(icon_id)
            if im is None:
                return 0xFFFF
            self.iidx[icon_id] = self.n_img + len(self.imgs)
            self.imgs.append(cu.to_t8(im))
        return self.iidx[icon_id]


def build_tables(dyn: Dyn) -> tuple[bytes, dict]:
    P = params()
    ci, pr, wp, gd, ac = (P[k] for k in ("CharaInitParam", "EquipParamProtector", "EquipParamWeapon",
                                         "EquipParamGoods", "EquipParamAccessory"))
    I = lambda f, i: _FMG.get(("item", f), {}).get(i)  # noqa: E731
    recs, info = [], dict(classes=[], gifts=[], hair=[])
    for k, row in enumerate(CLASS_ROWS):
        r = ci[row]
        attrs = [r.Vitality, r.Attunement, r.Endurance, r.Strength, r.Dexterity, r.Resistance, r.Intelligence, r.Faith]
        eq_s, eq_i, weight = [], [], 0.0
        for fld in ("RightHandWeapon1", "RightHandWeapon2", "LeftHandWeapon1", "LeftHandWeapon2"):
            wid = getattr(r, fld)
            if wid > 0 and wid in wp:
                eq_s.append(dyn.s(I(11, wid))); eq_i.append(dyn.icon(wp[wid].WeaponIcon)); weight += wp[wid].Weight
            else:
                eq_s.append(0xFFFF); eq_i.append(0xFFFF)
        for fld in ("HeadArmor", "BodyArmor", "HandsArmor", "LegsArmor"):
            pid = getattr(r, fld)
            if pid in pr and pr[pid].EquipmentModel != 0:
                eq_s.append(dyn.s(I(12, pid))); eq_i.append(dyn.icon(pr[pid].MaleIcon)); weight += pr[pid].Weight
            else:
                eq_s.append(0xFFFF); eq_i.append(0xFFFF)
        g_s, g_i, g_c, g_d = [0xFFFF, 0xFFFF], [0xFFFF, 0xFFFF], [0, 0], [0xFFFF, 0xFFFF]
        n_goods = 0
        for slot in (1, 2):
            gid = getattr(r, f"GoodSlot{slot}")
            if gid > 0 and gid in gd and n_goods < 2:
                g_s[n_goods] = dyn.s(I(10, gid)); g_i[n_goods] = dyn.icon(gd[gid].GoodIcon)
                g_c[n_goods] = getattr(r, f"GoodSlot{slot}Count")
                g_d[n_goods] = dyn.s(I(24, gid))
                n_goods += 1
        s_name, s_desc = dyn.s(fmg(("menu", 77, 132020 + k))), dyn.s(fmg(("menu", 77, 132320 + k)))
        bare = pr[r.HeadArmor].EquipmentModel == 0
        recs.append(struct.pack("<HH8B4BHH8H8H2H2H2H2Hf", row, r.Level, *attrs, r.Humanity, int(bare), n_goods, 0,
                                s_name, s_desc, *eq_s, *eq_i, *g_s, *g_i, *g_c, *g_d, weight))
        info["classes"].append(dict(row=row, name=fmg(("menu", 77, 132020 + k)), level=r.Level, attrs=attrs,
                                    bare_head=bare, weight=round(weight, 1)))
    grecs = []
    for k, row in enumerate(GIFT_ROWS):
        r = ci[row]
        s_name, s_desc = dyn.s(fmg(("menu", 77, 132050 + k))), dyn.s(fmg(("menu", 77, 132350 + k)))
        if r.GoodSlot1 > 0:
            gid = r.GoodSlot1
            ist, iim, cnt, idesc = dyn.s(I(10, gid)), dyn.icon(gd[gid].GoodIcon), r.GoodSlot1Count, dyn.s(I(24, gid))
        elif r.RingSlot1 > 0:
            aid = r.RingSlot1
            ist, iim, cnt, idesc = dyn.s(I(13, aid)), dyn.icon(ac[aid].MenuIcon), 1, dyn.s((I(27, aid) or "").strip())
        else:
            ist = iim = idesc = 0xFFFF; cnt = 0
        grecs.append(struct.pack("<6H", s_name, s_desc, ist, iim, cnt, idesc))
        info["gifts"].append(dict(row=row, name=fmg(("menu", 77, 132050 + k)), count=cnt))
    hrecs = []
    for gender, rows in HAIR.items():
        for k, row in enumerate(rows):
            nm = I(12, row) or ""
            base = 132420 if gender == 0 else 132430
            desc = fmg(("menu", 77, base + (row % 1000) // 100))
            hrecs.append(struct.pack("<2H2BH", dyn.s(nm), dyn.s(desc), gender, k, 0))
            info["hair"].append(dict(row=row, gender=gender, name=nm))
    hdr = struct.pack("<4sI4H", b"ASCL", 1, len(recs), len(grecs), len(hrecs), 0)
    return hdr + b"".join(recs) + b"".join(grecs) + b"".join(hrecs), info


# ------------------------------------------------------------------------------------------- main

def gen_header(path: Path):
    L = ["/* Generated by tools/cook_menu.py --gen-header: indices into menu.ashui strings / images. Do not edit. */",
         "#ifndef ASH_MENUSTR_H", "#define ASH_MENUSTR_H", "", "enum {"]
    L += [f"    {n}," for n, _ in STRINGS] + ["    MS_COUNT", "};", "", "enum {"]
    L += [f"    {n}," for n, _, _ in IMAGES] + ["    MI_COUNT", "};", "", "#endif", ""]
    path.write_text("\n".join(L))


LANG = "RUSSIAN"


def main():
    global LANG, _FMG
    ap = argparse.ArgumentParser()
    ap.add_argument("--out-dir")
    ap.add_argument("--lang", default="RUSSIAN")
    ap.add_argument("--png", help="previews (atlas, images) -> this dir (outside git)")
    ap.add_argument("--gen-header")
    a = ap.parse_args()
    if a.gen_header:
        gen_header(Path(a.gen_header))
        print("header:", a.gen_header)
        if not a.out_dir:
            return
    LANG = a.lang.upper()
    if not (DSR / "msg" / LANG).exists():
        print(f"no msg/{LANG}, falling back to ENGLISH", file=sys.stderr)
        LANG = "ENGLISH"
    _FMG = cu.load_fmgs(LANG)
    strings, src = [], {}
    for name, s in STRINGS:
        if s[0] == "OWN":
            strings.append(s[1]); src[name] = "own"
        else:
            strings.append(fmg(s)); src[name] = f"{LANG} {s[0]}.msgbnd fmg {s[1]} id {s[2]}"
    images = build_images()
    dyn = Dyn(len(strings), len(images))
    cls_blob, info = build_tables(dyn)
    strings += dyn.strs
    images += dyn.imgs
    font = build_font("".join(strings))
    missing = sorted({c for s in strings for c in s if c not in font["chars"] and c not in "\n\r\t"})
    out = Path(a.out_dir).expanduser()
    out.mkdir(parents=True, exist_ok=True)
    st = cu.write(out / "menu.ashui", font, strings, images)
    (out / "menu.ashcls").write_bytes(cls_blob)
    st.update(lang=LANG, fonts=[f"{Path(p).name} {s}px" for p, s, _ in FONTS], n_fixed_strings=len(STRINGS),
              n_dyn_strings=len(dyn.strs), n_fixed_images=len(IMAGES), n_dyn_images=len(dyn.imgs),
              ashcls_bytes=len(cls_blob), tables=info, string_source=src, missing_glyphs=missing,
              image_bytes=sum(len(i["pix"]) + len(i["clut"]) for i in images if i))
    st.pop("images", None)
    (out / "menu.json").write_text(json.dumps(st, ensure_ascii=False, indent=1))
    if a.png:
        p = Path(a.png).expanduser()
        p.mkdir(parents=True, exist_ok=True)
        Image.fromarray(font["atlas_preview"]).save(p / "menu_font.png")
        tiles = [i["preview"] for i in images if i]
        W = 512
        sheet = Image.new("RGBA", (W, 900), (60, 30, 60, 255))
        x = y = sh = 0
        for t in tiles:
            if x + t.width > W:
                x, y, sh = 0, y + sh + 2, 0
            sheet.alpha_composite(t.convert("RGBA"), (x, y))
            x += t.width + 2
            sh = max(sh, t.height)
        sheet.crop((0, 0, W, y + sh)).save(p / "menu_images.png")
    print(json.dumps({k: v for k, v in st.items() if k not in ("string_source", "tables")}, ensure_ascii=False,
                     indent=1)[:1500])
    print("classes:", [(c["name"], c["level"], c["attrs"]) for c in info["classes"]])
    print("OK" if not missing else f"MISSING GLYPHS: {missing}")


if __name__ == "__main__":
    main()
