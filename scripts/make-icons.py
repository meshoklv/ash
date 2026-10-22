#!/usr/bin/env python3
"""Draw the XMB icons (ICON0.PNG, 144x80) for our own EBOOTs. Needs Pillow (tools/.venv).

    tools/.venv/bin/python scripts/make-icons.py
-> src/bench/icon_bench.png, src/bench/icon_mem64.png, dist-res/icon_asylum.png
"""
import pathlib
from PIL import Image, ImageDraw, ImageFont

repo = pathlib.Path(__file__).resolve().parent.parent


def font(size):
    for p in ("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
              "/usr/share/fonts/dejavu/DejaVuSans-Bold.ttf"):
        if pathlib.Path(p).exists():
            return ImageFont.truetype(p, size)
    try:
        return ImageFont.load_default(size=size)
    except TypeError:
        return ImageFont.load_default()


def icon(path, title, sub, top, bottom):
    im = Image.new("RGB", (144, 80))
    d = ImageDraw.Draw(im)
    for y in range(80):
        t = y / 79
        d.line([(0, y), (143, y)], fill=tuple(int(top[i] * (1 - t) + bottom[i] * t) for i in range(3)))
    d.rectangle([0, 0, 143, 79], outline=(255, 140, 40), width=2)
    f1, f2 = font(22), font(13)
    w = d.textlength(title, font=f1)
    d.text(((144 - w) / 2, 14), title, font=f1, fill=(255, 220, 160))
    w = d.textlength(sub, font=f2)
    d.text(((144 - w) / 2, 48), sub, font=f2, fill=(230, 230, 230))
    path.parent.mkdir(parents=True, exist_ok=True)
    im.save(path, optimize=True)
    print(path, path.stat().st_size, "B")


icon(repo / "src/bench/icon_bench.png", "ASH BENCH", "PSP hardware test", (40, 20, 10), (10, 5, 5))
icon(repo / "src/bench/icon_mem64.png", "ASH MEM", "memory 64 MB mode", (10, 20, 40), (5, 5, 10))
icon(repo / "dist-res/icon_asylum.png", "ASH", "Asylum (viewer)", (30, 30, 30), (60, 25, 5))
