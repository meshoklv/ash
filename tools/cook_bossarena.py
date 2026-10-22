#!/usr/bin/env python3
"""Boss arena data for src/boss: the MSB regions the Asylum Demon's AI script reads + its name for the HP bar.

    tools/.venv/bin/python tools/cook_bossarena.py --out ~/ash-assets/cooked/boss_arena.ashbr

Regions (m18_01_00_00.msb, ids from 223200_battle.lua / the map's events): 1812350 balcony, 1812351/1812352 inner
balcony, 1812990 hall (BGM), 1812996 battle start, 1812301 move target (Act16), 1812305 drop point in the air,
1812300 spot after the drop, 1812320 escape door, 1812998 entrance, 1812311 / 1812400 floor break (pit, later).
Name: FMG NPC_name 2232 (msg/RUSSIAN/item.msgbnd) rendered with DejaVu Sans Bold into a PSM 4444 texture.

Output (FromSoftware-derived, never committed) "ASBR" v1, little-endian:
  header 20 B: "ASBR", u32 version=1, u32 n_region, u16 name_w, name_h, name_tw (texture width, pow2), u16 pad
  n_region x 36 B: s32 entity id, s32 shape (0 box: centre-bottom w x d x h, 1 sphere: r), f32 pos[3] (PSP space,
                   x mirrored), f32 size[3], f32 yaw (PSP radians = -FromSoft degrees)
  pad to 16, name texture name_tw x name_h u16 (GU_PSM_4444, ABGR nibbles: R low)
"""
from __future__ import annotations

import argparse
import logging
import math
import struct
import sys
from pathlib import Path

logging.disable(logging.WARNING)
import numpy as np  # noqa: E402
from PIL import Image, ImageDraw, ImageFont  # noqa: E402
from soulstruct.base.text.fmg import FMG  # noqa: E402
from soulstruct.containers import Binder  # noqa: E402
from soulstruct.darksouls1r.maps import MSB  # noqa: E402

sys.path.insert(0, str(Path(__file__).parent))
from dsrlib import DSR  # noqa: E402

REGIONS = [1812350, 1812351, 1812352, 1812990, 1812996, 1812301, 1812305, 1812300, 1812320, 1812998, 1812311,
           1812400]
NAME_ID = 2232
FONT = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"


def name_texture(text: str, px: int = 11) -> tuple[int, int, int, bytes]:
    font = ImageFont.truetype(FONT, px)
    x0, y0, x1, y1 = font.getbbox(text)
    w, h = x1 - x0 + 2, y1 - y0 + 2
    tw = 1
    while tw < w:
        tw *= 2
    th = 1
    while th < h:
        th *= 2
    im = Image.new("L", (tw, th), 0)
    d = ImageDraw.Draw(im)
    d.text((1 - x0 + 1, 1 - y0 + 1), text, font=font, fill=255)  # shadow
    sh = np.array(im, np.float32)
    im2 = Image.new("L", (tw, th), 0)
    ImageDraw.Draw(im2).text((1 - x0, 1 - y0), text, font=font, fill=255)
    fg = np.array(im2, np.float32)
    a = np.clip(np.maximum(fg, sh * 0.8), 0, 255)
    lum = np.where(fg > 0, 0.92, 0.0)  # off-white text, black shadow
    r = (lum * 15).round().astype(np.uint16)
    g = r
    bl = (lum * 0.95 * 15).round().astype(np.uint16)
    al = (a / 255 * 15).round().astype(np.uint16)
    pix = r | (g << 4) | (bl << 8) | (al << 12)
    return w, th, tw, pix.astype("<u2").tobytes()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--lang", default="RUSSIAN")
    args = ap.parse_args()
    msb = MSB.from_path(DSR / "map/MapStudio/m18_01_00_00.msb")
    regs = []
    for r in msb.regions:
        if r.entity_id not in REGIONS:
            continue
        p = [float(v) for v in r.translate]
        yaw = -math.radians(float(r.rotate[1]))
        sh = r.shape
        if type(sh).__name__ == "SphereShape":
            shape, size = 1, (float(sh.radius), 0.0, 0.0)
        else:
            shape, size = 0, (float(sh.width), float(sh.depth), float(sh.height))
        regs.append((r.entity_id, shape, (-p[0], p[1], p[2]), size, yaw, r.name))
    missing = sorted(set(REGIONS) - {x[0] for x in regs})
    name = ""
    for e in Binder.from_path(DSR / "msg" / args.lang / "item.msgbnd.dcx").entries:
        if e.name.endswith("NPC_name_.fmg"):
            name = FMG.from_bytes(e.data).entries.get(NAME_ID, "") or name
    w, h, tw, tex = name_texture(name)
    body = bytearray(struct.pack("<4s2I4H", b"ASBR", 1, len(regs), w, h, tw, 0))
    for rid, shape, pos, size, yaw, _ in regs:
        body += struct.pack("<2i7f", rid, shape, *pos, *size, yaw)
    while len(body) % 16:
        body.append(0)
    body += tex
    out = Path(args.out).expanduser()
    out.write_bytes(body)
    for rid, shape, pos, size, yaw, nm in regs:
        print(f"region {rid} {'sphere' if shape else 'box'} pos=({pos[0]:.2f} {pos[1]:.2f} {pos[2]:.2f}) "
              f"size={tuple(round(v, 2) for v in size)} yaw={math.degrees(yaw):.0f}")
    print(f"name {NAME_ID} [{args.lang}] = {name!r}: texture {tw}x{h}, text {w} px wide")
    print(f"wrote {out} ({len(body)} B), regions {len(regs)}/{len(REGIONS)}, missing {missing}")
    sys.exit(0 if not missing and name else 1)


if __name__ == "__main__":
    main()
