#!/usr/bin/env python3
"""Top-down map of the zone collision (debug aid for autopilot routes, src/game/asy_auto.c).

    tools/.venv/bin/python tools/pcol_map.py <zone.pak> <out.png> <x0> <z0> <x1> <z1> [--y Y0 Y1] [--fs] [--pt @x,z,label ...]

Coordinates: PSP space by default, --fs = FromSoft (x mirrored) for the box and the points. Walkable triangles are
filled by height (dark = low, bright = high), steep ones drawn grey. 20 px per metre, grid every metre (labels 5 m).
"""
import argparse
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

sys.path.insert(0, str(Path(__file__).parent))
from world_cams import load_pcol, pak_entry  # noqa: E402

ap = argparse.ArgumentParser()
ap.add_argument("pak"); ap.add_argument("out")
ap.add_argument("box", nargs=4, type=float)
ap.add_argument("--y", nargs=2, type=float, default=[-1e9, 1e9])
ap.add_argument("--fs", action="store_true")
ap.add_argument("--pt", nargs="*", default=[])
ap.add_argument("--ppm", type=float, default=20)
a = ap.parse_args()
x0, z0, x1, z1 = a.box
if a.fs:
    x0, x1 = -x1, -x0
pak = Path(a.pak).read_bytes()
V, T, names = load_pcol(pak_entry(pak, "pcol"))
P = V[T["v"].astype(np.int64)]
n = np.cross(P[:, 1] - P[:, 0], P[:, 2] - P[:, 0])
ny = n[:, 1] / (np.linalg.norm(n, axis=1) + 1e-12)
cy = P[:, :, 1].mean(1)
sel = (P[:, :, 0].max(1) >= x0) & (P[:, :, 0].min(1) <= x1) & (P[:, :, 2].max(1) >= z0) & (P[:, :, 2].min(1) <= z1)
sel &= (cy >= a.y[0]) & (cy <= a.y[1])
s = a.ppm
W, H = int((x1 - x0) * s), int((z1 - z0) * s)
img = Image.new("RGB", (W, H), (20, 20, 30))
d = ImageDraw.Draw(img)
idx = np.nonzero(sel)[0]
walk = idx[ny[idx] >= 0.64]
steep = idx[ny[idx] < 0.64]
ylo, yhi = (cy[walk].min(), cy[walk].max()) if len(walk) else (0, 1)
def px(p):
    return ((p[0] - x0) * s, (p[2] - z0) * s)
for i in steep:
    d.polygon([px(p) for p in P[i]], outline=(110, 110, 110))
for i in sorted(walk, key=lambda i: cy[i]):
    k = (cy[i] - ylo) / max(1e-3, yhi - ylo)
    col = (int(40 + 200 * k), int(90 + 120 * (1 - abs(k - 0.5) * 2)), int(220 - 180 * k))
    d.polygon([px(p) for p in P[i]], fill=col)
for gx in range(int(np.floor(x0)), int(np.ceil(x1)) + 1):
    d.line([((gx - x0) * s, 0), ((gx - x0) * s, H)], fill=(60, 60, 60) if gx % 5 else (160, 160, 160))
    if gx % 5 == 0:
        d.text(((gx - x0) * s + 2, 2), f"{-gx if a.fs else gx}", fill=(255, 255, 255))
for gz in range(int(np.floor(z0)), int(np.ceil(z1)) + 1):
    d.line([(0, (gz - z0) * s), (W, (gz - z0) * s)], fill=(60, 60, 60) if gz % 5 else (160, 160, 160))
    if gz % 5 == 0:
        d.text((2, (gz - z0) * s + 2), f"{gz}", fill=(255, 255, 255))
for t in a.pt:
    parts = t.lstrip("@").split(",")
    x, z = float(parts[0]), float(parts[1])
    if a.fs:
        x = -x
    lab = parts[2] if len(parts) > 2 else ""
    c = px((x, 0, z))
    d.ellipse([c[0] - 4, c[1] - 4, c[0] + 4, c[1] + 4], outline=(255, 0, 0), width=2)
    d.text((c[0] + 5, c[1] - 5), lab, fill=(255, 255, 0))
d.text((4, H - 14), f"y {ylo:.1f}..{yhi:.1f} (blue low, red high){' FS x' if a.fs else ''}", fill=(255, 255, 255))
img.save(a.out)
print(a.out, W, H, len(walk), "walkable", len(steep), "steep")
