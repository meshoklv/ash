#!/usr/bin/env python3
"""Cook the HUD / menu resources for src/ui/ into one `ui.ashui` (outside git: holds FromSoftware text and art).

    tools/.venv/bin/python tools/cook_ui.py --out ~/ash-assets/cooked/ui.ashui [--lang RUSSIAN|ENGLISH] [--png DIR]

What goes in (docs/progress-ui.md):
  * Font: DejaVu Serif (Bitstream Vera licence + public-domain changes: free to use, embed and redistribute;
    /usr/share/fonts/truetype/dejavu) rasterised at two sizes into one 4-bit alpha atlas (GE CLUT4, 16 levels).
    Glyph set = ASCII + full Russian alphabet + a few typographic signs + every char of the strings below.
  * Strings: the game's own FMG text from msg/<LANG> (menu.msgbnd / item.msgbnd), looked up by (bnd, fmg, id), plus
    a few lines of our own where DSR has no text (tagged OWN below, "перевод свой").
  * Banners: "ВЫ ПОГИБЛИ", "ВРАГ ПОВЕРЖЕН", "КОСТЕР ЗАЖЖЕН", "Северное Прибежище Нежити" are textures in DSR
    (menu/RUSSIAN/menu_local.tpf: Menu14, Menu15, Menu20), cut out by their alpha inside a band and scaled to
    PSP size, 8-bit CLUT. Only for RUSSIAN (the English atlas has another layout) - without a banner the UI
    draws the same string with the font.
  * Icons: Estus Flask full / empty (EquipParamGoods 201/200 GoodIcon 2005/2004 -> menu_0.tpf Icon20, 12x12 grid,
    index = icon id - 2000) at 32x32.

`.ashui` v1, little-endian, all offsets from file start, every section 16-aligned (C side: src/ui/ui.h):
  Header 64 B : "ASHU", u32 version=1, u16 atlas_w, u16 atlas_h, u32 off_atlas (T4, low nibble = left pixel),
                u32 n_font, u32 off_font, u32 n_glyph, u32 off_glyph, u32 n_str, u32 off_str, u32 n_img, u32 off_img,
                u32 pad[4]
  Font  8 B   : u16 line_h, u16 ascent, u16 first_glyph, u16 n_glyph   (glyphs of a font sorted by code point)
  Glyph 12 B  : u16 cp, u16 x, u16 y, u8 w, u8 h, s8 xoff, s8 yoff (from the pen at the baseline), u8 adv, u8 pad
  Str         : n_str u32 offsets to zero-terminated UTF-8 (0 = missing)
  Img  16 B   : u16 tw, u16 th (texture, power of two), u16 w, u16 h (content), u32 off_clut (256 x u32 ABGR),
                u32 off_pix (T8, tw * th)
String and image indices are the enums in src/ui/uistr.h (kept in sync by --gen-header).
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

logging.disable(logging.WARNING)
import numpy as np  # noqa: E402
from PIL import Image, ImageFont  # noqa: E402

DSR = Path(os.environ.get("ASH_DSR", Path.home() / "ash-assets/dsr"))
FONT_DIR = Path("/usr/share/fonts/truetype/dejavu")
FONT_FILE = "DejaVuSerif.ttf"
FONT_SIZES = (12, 17)          # px: 0 = HUD / prompts / menu, 1 = titles, boss name [придумал: по глазу на 480x272]

# (enum name, source). Source: ("menu"|"item", fmg entry id, text id) from the game, or ("OWN", "text").
STRINGS = [
    ("S_REST", ("menu", 30, 10010183)),          # Отдохнуть у костра
    ("S_LIGHT", ("menu", 30, 10010182)),         # Разжечь костер
    ("S_LEAVE", ("menu", 30, 15000005)),         # Выйти
    ("S_LEVELUP", ("menu", 30, 15000100)),       # Новый уровень
    ("S_PICKUP", ("menu", 30, 10010103)),        # Забрать предмет
    ("S_OPEN", ("menu", 30, 10010400)),          # Открыть
    ("S_FOG", ("menu", 30, 10010403)),           # Пройти через белый свет
    ("S_LOCKED", ("menu", 30, 10010163)),        # Заперто.
    ("S_EXAMINE", ("menu", 30, 10010100)),       # Осмотреть
    ("S_PULL_LEVER", ("menu", 30, 10010501)),
    ("S_KICK_LADDER", ("menu", 30, 10010500)),
    ("S_CLIMB_UP", ("menu", 30, 10010300)),
    ("S_CLIMB_DOWN", ("menu", 30, 10010301)),
    ("S_SOULS", ("menu", 76, 101041)),           # Души
    ("S_ESTUS_HELP", ("menu", 76, 303002)),      # Количество использований. Восстанавливается после отдыха у костра.
    ("S_LOADING", ("menu", 92, 47102)),          # Загрузка...\nНе отключайте питание.
    ("S_LOADING_SAVE", ("menu", 92, 47100)),     # Загрузка сохраненных данных.\nНе отключайте питание.
    ("S_ESTUS", ("item", 10, 201)),              # Фляга с Эстусом
    ("S_BOSS_ASYLUM", ("item", 18, 2232)),       # Демон Прибежища
    ("S_AREA_ASYLUM", ("item", 19, 1810)),       # Северное Прибежище Нежити
    ("S_DIED", ("OWN", "ВЫ ПОГИБЛИ")),           # in DSR a texture (Menu14), text = fallback
    ("S_VICTORY", ("OWN", "ВРАГ ПОВЕРЖЕН")),      # texture (Menu14)
    ("S_BONFIRE_LIT", ("OWN", "КОСТЕР ЗАЖЖЕН")),  # texture (Menu15)
    ("S_SAVING", ("OWN", "Сохранение...")),      # OWN: DSR saves silently (icon only)
    ("S_SAVED_OK", ("OWN", "Игра сохранена")),   # OWN: debug toast
    ("S_SOULS_BACK", ("OWN", "Души возвращены")),  # OWN: DS1 shows nothing, we toast once [придумал]
    ("S_EMPTY", ("OWN", "")),
]

# Banners from DSR menu atlases (RUSSIAN): (enum, texture, band x0, y0, x1, y1 in the 2048 atlas, target width)
BANNERS = [
    ("IMG_DIED", "Menu14", (0, 975, 2048, 1195), 240),
    ("IMG_VICTORY", "Menu14", (0, 1200, 2048, 1376), 240),
    ("IMG_BONFIRE_LIT", "Menu15", (0, 1220, 2048, 1368), 220),
    ("IMG_AREA_ASYLUM", "Menu20", (0, 1836, 1034, 1980), 250),
]
ICONS = [("IMG_ESTUS", 2005), ("IMG_ESTUS_EMPTY", 2004)]
IMAGES = [b[0] for b in BANNERS] + [i[0] for i in ICONS]

EXTRA_CHARS = "«»—–…№°×·’“”"


def align(b: bytearray, n: int = 16):
    while len(b) % n:
        b.append(0)


# ------------------------------------------------------------------------------------------- text

def load_fmgs(lang: str) -> dict:
    from soulstruct.base.text.fmg import FMG
    from soulstruct.containers import Binder
    out = {}
    for bnd in ("menu", "item"):
        b = Binder.from_path(DSR / "msg" / lang / f"{bnd}.msgbnd.dcx")
        for e in b.entries:
            if (bnd, e.entry_id) in out:
                continue
            f = FMG.from_bytes(e.data)
            items = f.entries.items() if hasattr(f.entries, "items") else [(x.id, x.text) for x in f.entries]
            out[(bnd, e.entry_id)] = {int(k): v for k, v in items if v}
    return out


def build_strings(lang: str) -> tuple[list[str], dict]:
    fmgs = load_fmgs(lang)
    res, src = [], {}
    for name, s in STRINGS:
        if s[0] == "OWN":
            txt = s[1] if lang == "RUSSIAN" else {"S_DIED": "YOU DIED", "S_VICTORY": "VICTORY ACHIEVED",
                                                  "S_BONFIRE_LIT": "BONFIRE LIT", "S_SAVING": "Saving...",
                                                  "S_SAVED_OK": "Game saved", "S_SOULS_BACK": "Souls retrieved"
                                                  }.get(name, s[1])
            src[name] = "own"
        else:
            txt = fmgs.get((s[0], s[1]), {}).get(s[2])
            if txt is None:
                raise SystemExit(f"FMG text missing: {name} {s}")
            src[name] = f"{lang} {s[0]}.msgbnd fmg {s[1]} id {s[2]}"
        res.append(txt.replace("\r", ""))
    return res, src


# ------------------------------------------------------------------------------------------- font

def build_font(extra_text: str):
    chars = set(chr(c) for c in range(32, 127))
    chars |= set(chr(c) for c in range(0x410, 0x450)) | {"Ё", "ё"}
    chars |= set(EXTRA_CHARS) | set(extra_text)
    chars -= {"\n", "\r", "\t"}
    chars = sorted(chars, key=ord)
    glyphs, fonts = [], []
    for size in FONT_SIZES:
        f = ImageFont.truetype(str(FONT_DIR / FONT_FILE), size)
        asc, desc = f.getmetrics()
        first = len(glyphs)
        for ch in chars:
            if ch == " ":
                glyphs.append(dict(cp=32, img=None, w=0, h=0, xoff=0, yoff=0, adv=round(f.getlength(" "))))
                continue
            bbox = f.getbbox(ch, anchor="ls")          # relative to the pen on the baseline
            if bbox[2] <= bbox[0] or bbox[3] <= bbox[1]:
                glyphs.append(dict(cp=ord(ch), img=None, w=0, h=0, xoff=0, yoff=0, adv=round(f.getlength(ch))))
                continue
            im = Image.new("L", (bbox[2] - bbox[0], bbox[3] - bbox[1]), 0)
            from PIL import ImageDraw
            ImageDraw.Draw(im).text((-bbox[0], -bbox[1]), ch, font=f, fill=255, anchor="ls")
            glyphs.append(dict(cp=ord(ch), img=np.asarray(im), w=im.width, h=im.height, xoff=bbox[0], yoff=bbox[1],
                               adv=round(f.getlength(ch))))
        fonts.append(dict(line_h=asc + desc, ascent=asc, first=first, n=len(glyphs) - first))
    # shelf packing into a 256-wide atlas, 1 px gap
    W = 256
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
    atlas = np.zeros((H, W), np.uint8)
    for g in glyphs:
        if g["img"] is not None:
            atlas[g["y"]:g["y"] + g["h"], g["x"]:g["x"] + g["w"]] = g["img"]
    a4 = ((atlas.astype(np.uint32) * 15 + 127) // 255).astype(np.uint8)
    packed = (a4[:, 0::2] | (a4[:, 1::2] << 4)).astype(np.uint8)   # low nibble = left pixel (GE T4)
    return dict(W=W, H=H, pix=packed.tobytes(), glyphs=glyphs, fonts=fonts, atlas_preview=a4 * 17, chars=chars)


# ------------------------------------------------------------------------------------------- images

def tpf_image(path: Path, stem: str) -> Image.Image:
    from soulstruct.containers import TPF
    t = TPF.from_path(path)
    x = next(t for t in t.textures if t.stem == stem)
    im = Image.open(io.BytesIO(x.data))
    im.load()
    return im.convert("RGBA")


def luma_key(im: Image.Image) -> Image.Image:
    """Text drawn on an opaque black band (Menu14 "ВЫ ПОГИБЛИ"): alpha from brightness, colour un-premultiplied."""
    a = np.asarray(im).astype(np.float32)
    h, w = a.shape[:2]
    corners = [a[2, 2, 3], a[2, w - 3, 3], a[h - 3, 2, 3], a[h - 3, w - 3, 3]]
    if min(corners) < 200:
        return im
    m = a[:, :, :3].max(axis=2)
    alpha = np.clip(m / max(1.0, np.percentile(m, 99.5)) * 1.15, 0, 1)
    rgb = np.where(alpha[..., None] > 0.02, a[:, :, :3] / np.maximum(alpha[..., None], 0.02), 0)
    out = np.dstack([np.clip(rgb, 0, 255), alpha * 255]).astype(np.uint8)
    return Image.fromarray(out, "RGBA")


def to_t8(im: Image.Image) -> dict:
    """RGBA -> power-of-two T8 + 256-entry ABGR CLUT (premultiplied-free; index 0 = transparent)."""
    w, h = im.size
    tw, th = 1, 1
    while tw < w:
        tw *= 2
    while th < h:
        th *= 2
    tw = max(tw, 16)
    a = np.asarray(im).copy()
    a[a[:, :, 3] < 8] = 0
    q = Image.fromarray(a, "RGBA").quantize(colors=255, method=Image.Quantize.FASTOCTREE, dither=Image.Dither.NONE)
    pal = np.array(q.getpalette(rawmode="RGBA")[:255 * 4], np.uint8).reshape(-1, 4)
    idx = np.asarray(q).astype(np.uint16) + 1
    idx[a[:, :, 3] == 0] = 0
    clut = np.zeros((256, 4), np.uint8)
    clut[1:1 + len(pal)] = pal
    pix = np.zeros((th, tw), np.uint8)
    pix[:h, :w] = idx
    abgr = (clut[:, 3].astype(np.uint32) << 24) | (clut[:, 2].astype(np.uint32) << 16) | \
           (clut[:, 1].astype(np.uint32) << 8) | clut[:, 0].astype(np.uint32)
    return dict(tw=tw, th=th, w=w, h=h, clut=abgr.astype("<u4").tobytes(), pix=pix.tobytes(), preview=im)


def build_images(lang: str) -> list:
    imgs = []
    have_ru = lang == "RUSSIAN" and (DSR / "menu/RUSSIAN/menu_local.tpf.dcx").exists()
    cache = {}
    for name, stem, (x0, y0, x1, y1), tw in BANNERS:
        if not have_ru:
            imgs.append(None)
            continue
        if stem not in cache:
            cache[stem] = tpf_image(DSR / "menu/RUSSIAN/menu_local.tpf.dcx", stem)
        band = luma_key(cache[stem].crop((x0, y0, x1, y1)))
        al = np.asarray(band)[:, :, 3]
        ys, xs = np.nonzero(al > 24)
        crop = band.crop((max(0, xs.min() - 4), max(0, ys.min() - 4), min(band.width, xs.max() + 5),
                          min(band.height, ys.max() + 5)))
        h = max(1, round(crop.height * tw / crop.width))
        imgs.append(to_t8(crop.resize((tw, h), Image.Resampling.LANCZOS)))
    icons = tpf_image(DSR / "menu/menu_0.tpf.dcx", "Icon20")
    cell = icons.width / 12
    for name, icon in ICONS:
        k = icon - 2000
        r, c = divmod(k, 12)
        # icons are not centred in their 170.7 px cells (measured: row 0 starts at 13, 177, 353, 521, 684, 830 ...):
        # take the run of opaque columns nearest to the cell centre and a square around it
        al = np.asarray(icons)[round(r * cell):round((r + 1) * cell), :, 3]
        occ = np.r_[False, al.max(axis=0) > 30, False]
        edges = np.flatnonzero(occ[1:] != occ[:-1])
        runs = list(zip(edges[0::2], edges[1::2]))
        cx = (c + 0.5) * cell
        x0, x1 = min(runs, key=lambda q: abs((q[0] + q[1]) / 2 - cx))
        rows = np.flatnonzero(al[:, x0:x1].max(axis=1) > 30)
        side = max(x1 - x0, rows[-1] - rows[0]) + 8
        mx, my = (x0 + x1) / 2, r * cell + (rows[0] + rows[-1]) / 2
        crop = icons.crop((round(mx - side / 2), round(my - side / 2), round(mx + side / 2), round(my + side / 2)))
        imgs.append(to_t8(crop.resize((32, 32), Image.Resampling.LANCZOS)))
    return imgs


# ------------------------------------------------------------------------------------------- writer

def write(out: Path, font: dict, strings: list[str], imgs: list) -> dict:
    body = bytearray(b"\0" * 64)
    off_atlas = len(body)
    body += font["pix"]; align(body)
    off_font = len(body)
    for f in font["fonts"]:
        body += struct.pack("<4H", f["line_h"], f["ascent"], f["first"], f["n"])
    align(body)
    off_glyph = len(body)
    for g in font["glyphs"]:
        body += struct.pack("<3H2B2bBB", g["cp"], g["x"], g["y"], g["w"], g["h"], g["xoff"], g["yoff"], g["adv"], 0)
    align(body)
    off_str = len(body)
    body += b"\0" * (4 * len(strings)); align(body)
    soffs = []
    for s in strings:
        soffs.append(len(body))
        body += s.encode("utf-8") + b"\0"
    body[off_str:off_str + 4 * len(strings)] = struct.pack(f"<{len(strings)}I", *soffs)
    align(body)
    off_img = len(body)
    body += b"\0" * (16 * len(imgs)); align(body)
    recs = []
    for im in imgs:
        if im is None:
            recs.append(b"\0" * 16)
            continue
        align(body); oc = len(body); body += im["clut"]
        align(body); op = len(body); body += im["pix"]
        recs.append(struct.pack("<4H2I", im["tw"], im["th"], im["w"], im["h"], oc, op))
    body[off_img:off_img + 16 * len(imgs)] = b"".join(recs)
    align(body)
    body[:64] = struct.pack("<4sI2H9I4I", b"ASHU", 1, font["W"], font["H"], off_atlas, len(font["fonts"]), off_font,
                            len(font["glyphs"]), off_glyph, len(strings), off_str, len(imgs), off_img, 0, 0, 0, 0)
    assert body[off_atlas:off_atlas + 16] == font["pix"][:16], "section offsets shifted"
    out.write_bytes(body)
    return dict(bytes=len(body), atlas=f"{font['W']}x{font['H']} T4 ({len(font['pix'])} B)",
                glyphs=len(font["glyphs"]), fonts=font["fonts"],
                images=[None if i is None else dict(tex=f"{i['tw']}x{i['th']}", content=f"{i['w']}x{i['h']}")
                        for i in imgs])


def gen_header(path: Path):
    L = ["/* Generated by tools/cook_ui.py --gen-header: indices into `.ashui` strings / images. Do not edit. */",
         "#ifndef ASH_UISTR_H", "#define ASH_UISTR_H", "", "enum {"]
    L += [f"    {n}," for n, _ in STRINGS] + ["    S_COUNT", "};", "", "enum {"]
    L += [f"    {n}," for n in IMAGES] + ["    IMG_COUNT", "};", "", "#endif", ""]
    path.write_text("\n".join(L))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out")
    ap.add_argument("--lang", default="RUSSIAN")
    ap.add_argument("--png", help="write previews (atlas, banners) to this dir (outside git)")
    ap.add_argument("--gen-header", help="write the C enum header (src/ui/uistr.h)")
    a = ap.parse_args()
    if a.gen_header:
        gen_header(Path(a.gen_header))
        print("header:", a.gen_header)
        if not a.out:
            return
    lang = a.lang.upper()
    if not (DSR / "msg" / lang).exists():
        print(f"no msg/{lang} in {DSR}, falling back to ENGLISH", file=sys.stderr)
        lang = "ENGLISH"
    strings, src = build_strings(lang)
    font = build_font("".join(strings))
    missing = sorted({c for s in strings for c in s if c not in font["chars"] and c not in "\n\r\t"})
    imgs = build_images(lang)
    out = Path(a.out).expanduser()
    out.parent.mkdir(parents=True, exist_ok=True)
    st = write(out, font, strings, imgs)
    st.update(lang=lang, font=f"{FONT_FILE} {FONT_SIZES}", strings={n: s for (n, _), s in zip(STRINGS, strings)},
              string_source=src, missing_glyphs=missing)
    out.with_suffix(".json").write_text(json.dumps(st, ensure_ascii=False, indent=1))
    if a.png:
        p = Path(a.png).expanduser()
        p.mkdir(parents=True, exist_ok=True)
        Image.fromarray(font["atlas_preview"]).save(p / "font_atlas.png")
        for n, im in zip(IMAGES, imgs):
            if im is not None:
                im["preview"].save(p / f"{n.lower()}.png")
    print(json.dumps({k: v for k, v in st.items() if k != "string_source"}, ensure_ascii=False, indent=1))
    print("OK" if not missing else f"MISSING GLYPHS: {missing}")


if __name__ == "__main__":
    main()
