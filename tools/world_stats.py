#!/usr/bin/env python3
"""Offline per-camera draw estimate of a zone pak (same culling as src/world/world.c: draw groups, chunk LOD,
far distance, frustum on batch spheres). Splits triangles by kind, so budgets can be tuned without the emulator.

    tools/.venv/bin/python tools/world_stats.py zone_m18_01.pak cams.txt [far lod]
"""
import math
import struct
import sys

import numpy as np

sys.path.insert(0, __import__("os").path.dirname(__file__))
from world_cams import pak_entry  # noqa: E402


def main():
    pak = open(sys.argv[1], "rb").read()
    far = float(sys.argv[3]) if len(sys.argv) > 3 else 50.0
    lod = float(sys.argv[4]) if len(sys.argv) > 4 else 8.0
    w = pak_entry(pak, "world"); z = pak_entry(pak, "ents")
    h = struct.unpack_from("<4s7I8I3f3ff4x", w, 0)
    nb, off_b, chunk = h[3], h[9], h[22]
    B = np.frombuffer(w, np.dtype([("tex", "<u2"), ("flags", "<u2"), ("vs", "<u4"), ("vc", "<u4"), ("is", "<u4"),
                                   ("ic", "<u4"), ("off", "<f4", 3), ("sc", "<f4", 3), ("uo", "<f4", 2),
                                   ("us", "<f4", 2), ("c", "<f4", 3), ("r", "<f4"), ("mask", "<u4", 4),
                                   ("chunk", "<u4")]), nb, off_b)
    zh = struct.unpack_from("<4s7I5I12x", z, 0)
    cols = {}
    for i in range(zh[2]):
        nm = z[zh[8] + 64 * i: zh[8] + 64 * i + 16].rstrip(b"\0").decode()
        cols[nm] = np.frombuffer(z, "<u4", 4, zh[8] + 64 * i + 16)
    ck = B["chunk"].astype(np.int64)
    cc = np.stack([(ck & 1023) - 512, ((ck >> 10) & 1023) - 512, ((ck >> 20) & 1023) - 512], 1) * chunk + chunk / 2
    for line in open(sys.argv[2]):
        if line.startswith("#") or not line.strip():
            continue
        p = line.split()
        eye, at, fov, cell = np.array(p[1:4], float), np.array(p[4:7], float), float(p[7]), p[8]
        disp = np.full(4, 0xFFFFFFFF, np.uint32) if cell not in cols else cols[cell]
        f = B["flags"]
        world = (f & (4 | 16)) == 0
        hit = (B["mask"] & disp).any(1) | ((f & 8) != 0)
        dist = np.linalg.norm(B["c"] - eye, axis=1)
        near = np.linalg.norm(np.maximum(np.abs(cc - eye) - chunk / 2, 0), axis=1) < lod   # eye to chunk box
        lodok = np.where(f & 64, near, np.where(f & 128, ~near, True))
        farok = (dist - B["r"] < np.where(f & 256, 24.0, far)) | ((f & 8) != 0)
        # frustum (approx: cone with half-angle of the diagonal)
        d = at - eye; d /= np.linalg.norm(d)
        half = math.atan(math.tan(math.radians(fov) / 2) * math.hypot(1, 480 / 272))
        v = B["c"] - eye
        dd = np.linalg.norm(v, axis=1) + 1e-6
        ang = np.arccos(np.clip((v @ d) / dd, -1, 1))
        fr = (ang - np.arcsin(np.clip(B["r"] / dd, 0, 1)) < half) | (dd < B["r"])
        sel = world & hit & lodok & farok & fr
        tris = B["ic"] // 3
        out = {k: int(tris[sel & m].sum()) for k, m in (("near", (f & 64) != 0), ("far", (f & 128) != 0),
                                                         ("decor", (f & (64 | 128 | 8)) == 0), ("bg", (f & 8) != 0))}
        print(f"{p[0]:14s} total {int(tris[sel].sum()):6d} batches {int(sel.sum()):4d}  {out}")


if __name__ == "__main__":
    main()
