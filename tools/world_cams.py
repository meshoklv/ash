#!/usr/bin/env python3
"""Camera tour of a cooked zone for src/world/test.c: writes cams.txt (PSP space + visibility cell per camera).

    tools/.venv/bin/python tools/world_cams.py ~/ash-assets/cooked/world/zone_m18_01.pak cams.txt [--only 0,3]

Cameras are given in FromSoft space (MSB coordinates, docs/world.md has where they come from) and mirrored here.
The visibility cell = the collision part under the point the player would stand on (`at` by default, `eye` with
cell_from="eye"), found by a vertical ray into the pak's .pcol.
"""
from __future__ import annotations

import struct
import sys
from pathlib import Path

import numpy as np

# label, eye (FS), target (FS), fov, cell_from ("at" | "eye" | "*")
TOUR = [
    ("cell", (-14.6, 186.7, -47.6), (-13.6, 185.6, -42.5), 70, "eye"),
    ("cell_door", (-13.9, 186.4, -40.2), (-13.9, 185.6, -45.5), 62, "eye"),
    ("corridor_s", (-14.2, 186.9, -38.5), (-14.6, 185.8, -22.0), 62, "eye"),
    ("corridor_n", (-14.3, 187.0, -18.0), (-14.0, 186.0, -4.0), 62, "eye"),
    ("hollow_room", (-14.0, 191.5, 13.0), (-8.0, 190.0, 22.0), 64, "at"),
    ("ladder", (-4.0, 191.5, 18.5), (-0.3, 193.0, 22.0), 64, "eye"),
    ("yard_bonfire", (4.5, 197.9, 11.0), (3.1, 196.4, 7.4), 62, "at"),
    ("yard_door", (3.1, 199.2, 12.0), (3.2, 199.0, -4.0), 62, "at"),
    ("yard_wide", (12.0, 201.5, 14.0), (-6.0, 197.0, -2.0), 64, "at"),
    ("hall_entry", (3.2, 200.5, -6.0), (3.0, 199.0, -29.0), 64, "eye"),
    ("hall_back", (3.2, 201.0, -31.0), (3.2, 199.5, -8.0), 64, "eye"),
    ("hall_gate", (8.0, 200.0, -24.0), (15.4, 199.0, -25.5), 62, "eye"),
    ("east_way", (19.0, 199.5, -25.5), (28.0, 196.5, -25.5), 62, "eye"),
    ("bonfire2", (36.0, 194.6, -21.5), (33.3, 193.6, -26.5), 64, "at"),
    ("east_rooms", (36.0, 194.5, -8.0), (36.0, 193.5, 10.0), 64, "eye"),
    ("balcony", (3.2, 210.4, -37.5), (3.2, 207.0, -27.0), 64, "eye"),
    ("plunge_view", (3.2, 210.5, -30.5), (3.2, 198.5, -18.0), 64, "eye"),
    ("upper_hall", (-7.9, 210.0, -26.0), (-7.9, 209.0, -6.0), 62, "eye"),
    ("crow_path", (3.2, 212.0, -60.0), (3.2, 212.0, -90.0), 64, "eye"),
    ("crow_nest", (3.3, 213.5, -86.0), (2.7, 212.5, -96.0), 64, "eye"),
    ("asylum_back", (3.3, 213.0, -88.0), (3.2, 210.0, -40.0), 64, "eye"),
    ("fog_balcony", (3.2, 210.6, -39.0), (3.2, 210.4, -34.0), 64, "eye"),
    ("gate_close", (10.5, 199.6, -25.5), (15.4, 199.3, -25.5), 62, "eye"),
    ("overview", (60.0, 245.0, -70.0), (5.0, 200.0, -10.0), 60, "*"),
    # the hall floor over the pit (o8510 flagstones under the Demon), boss-fight camera height
    ("hall_floor", (0.5, 200.6, -16.0), (3.2, 197.5, -22.0), 64, "at"),
]


def pak_entry(pak: bytes, name: str) -> bytes:
    n = struct.unpack_from("<4s3I", pak, 0)[2]
    for i in range(n):
        nm, off, size = struct.unpack_from("<16s2I", pak, 16 + 32 * i)
        if nm.rstrip(b"\0").decode() == name:
            return pak[off:off + size]
    raise KeyError(name)


def load_pcol(pcol: bytes):
    h = struct.unpack_from("<4s7I4f8I", pcol, 0)
    n_vert, n_tri, n_part = h[2], h[3], h[4]
    off_vert, off_tri, off_part = h[12], h[13], h[14]
    V = np.frombuffer(pcol, "<f4", n_vert * 3, off_vert).reshape(-1, 3)
    T = np.frombuffer(pcol, np.dtype([("v", "<u2", 3), ("m", "<u2"), ("p", "u1"), ("f", "u1")]), n_tri, off_tri)
    names = [struct.unpack_from("<16s", pcol, off_part + 24 * i)[0].rstrip(b"\0").decode() for i in range(n_part)]
    return V, T, names


def zone_cols(pak: bytes) -> set:
    z = pak_entry(pak, "ents")
    h = struct.unpack_from("<4s7I5I12x", z, 0)
    return {z[h[8] + 64 * i: h[8] + 64 * i + 16].rstrip(b"\0").decode() for i in range(h[2])}


def floor_part(V, T, names, x, y, z, up=1.0, down=8.0):
    P = V[T["v"].astype(np.int64)]
    a, b, c = P[:, 0], P[:, 1], P[:, 2]
    n = np.cross(b - a, c - a)
    ok = n[:, 1] / (np.linalg.norm(n, axis=1) + 1e-12) > 0.5
    d = (b[:, 2] - c[:, 2]) * (a[:, 0] - c[:, 0]) + (c[:, 0] - b[:, 0]) * (a[:, 2] - c[:, 2])
    d = np.where(np.abs(d) < 1e-12, 1e-12, d)
    w0 = ((b[:, 2] - c[:, 2]) * (x - c[:, 0]) + (c[:, 0] - b[:, 0]) * (z - c[:, 2])) / d
    w1 = ((c[:, 2] - a[:, 2]) * (x - c[:, 0]) + (a[:, 0] - c[:, 0]) * (z - c[:, 2])) / d
    w2 = 1 - w0 - w1
    inside = ok & (w0 >= -1e-4) & (w1 >= -1e-4) & (w2 >= -1e-4)
    h = w0 * a[:, 1] + w1 * b[:, 1] + w2 * c[:, 1]
    cand = inside & (h <= y + up) & (h >= y - down)
    if not cand.any():
        return None, None
    i = np.where(cand)[0][np.argmax(h[cand])]
    return names[T["p"][i]], float(h[i])


def main():
    pak = Path(sys.argv[1]).expanduser().read_bytes()
    out = Path(sys.argv[2])
    only = None
    if "--only" in sys.argv:
        only = {int(v) for v in sys.argv[sys.argv.index("--only") + 1].split(",")}
    V, T, names = load_pcol(pak_entry(pak, "pcol"))
    cols = zone_cols(pak)
    # parts that are not MSB collisions (floor plates of objects) carry no display groups: not a cell
    names = [n if n in cols else None for n in names]
    lines = ["# label ex ey ez tx ty tz fov cell   (PSP space, tools/world_cams.py)"]
    for i, (label, eye, at, fov, src) in enumerate(TOUR):
        if only is not None and i not in only:
            continue
        e = np.array(eye) * [-1, 1, 1]
        t = np.array(at) * [-1, 1, 1]
        cell = "*"
        if src != "*":
            p = t if src == "at" else e
            cell, h = floor_part(V, T, names, p[0], p[1], p[2])
            if cell is None:
                p = e if src == "at" else t
                cell, h = floor_part(V, T, names, p[0], p[1], p[2])
            cell = cell or "*"
        lines.append(f"{label} {e[0]:.2f} {e[1]:.2f} {e[2]:.2f} {t[0]:.2f} {t[1]:.2f} {t[2]:.2f} {fov} {cell}")
    out.write_text("\n".join(lines) + "\n")
    print("\n".join(lines))


if __name__ == "__main__":
    main()
