#!/usr/bin/env python3
"""Check that the cooked hit dummies follow the real animation: the knight's skeleton in a clip (same math as the PSP
runtime: tools/cook_knight.py reconstruct), the Longsword hit capsule built exactly as src/combat does it
(bone world x CbDummy.local), and a hollow standing in front. Prints at which frames the R1 capsule reaches a target
at several distances and draws side/top views.

    tools/.venv/bin/python tools/combat_pose_check.py [--clip a23_3000] [--out ~/ash-assets/shots/combat]

Reads ~/ash-assets/cooked/combat.ashcb (tools/cook_combat.py). Output images are derived from DSR -> outside git.
"""
from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

sys.path.insert(0, str(Path(__file__).parent))
import cook_knight as ck  # noqa: E402


def read_cb(path: Path):
    b = path.read_bytes()
    nsec = struct.unpack_from("<I", b, 8)[0]
    secs = [struct.unpack_from("<2I", b, 16 + 8 * i) for i in range(nsec)]
    off, n = secs[0]
    kinds = []
    for i in range(n):
        name, a0, na, d0, nd, h0, nh = struct.unpack_from("<8s6H", b, off + 84 * i)
        kinds.append((name.rstrip(b"\0").decode(), a0, na, d0, nd, h0, nh))
    doff, dn = secs[6]
    dummies = []
    for i in range(dn):
        rid, src, lx, ly, lz, mx, my, mz, bone = struct.unpack_from("<hBx3f3f20s", b, doff + 48 * i)
        dummies.append((rid, src, np.array([lx, ly, lz]), np.array([mx, my, mz]), bone.rstrip(b"\0").decode()))
    hoff, hn = secs[7]
    hurts = []
    for i in range(hn):
        a, bb, r, *m = struct.unpack_from("<20s20sf3f3f", b, hoff + 68 * i)
        hurts.append((a.rstrip(b"\0").decode(), bb.rstrip(b"\0").decode(), r, np.array(m[:3]), np.array(m[3:])))
    aoff, an = secs[5]
    return kinds, dummies, hurts, b, secs


def seg_dist(p1, q1, p2, q2):
    d1, d2, r = q1 - p1, q2 - p2, p1 - p2
    a, e, f = d1 @ d1, d2 @ d2, d2 @ r
    c, bb = d1 @ r, d1 @ d2
    den = a * e - bb * bb
    s = np.clip((bb * f - c * e) / den, 0, 1) if den > 1e-12 else 0.0
    t = (bb * s + f) / e if e > 1e-12 else 0.0
    if t < 0:
        t, s = 0.0, np.clip(-c / a, 0, 1) if a > 1e-12 else 0.0
    elif t > 1:
        t, s = 1.0, np.clip((bb - c) / a, 0, 1) if a > 1e-12 else 0.0
    return float(np.linalg.norm((p1 + d1 * s) - (p2 + d2 * t)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--clip", default="a23_3000")
    ap.add_argument("--window", default="16,20", help="TAE hit window frames start,end")
    ap.add_argument("--cooked", default=str(Path.home() / "ash-assets/cooked/combat.ashcb"))
    ap.add_argument("--out", default=str(Path.home() / "ash-assets/shots/combat"))
    args = ap.parse_args()
    out = Path(args.out).expanduser(); out.mkdir(parents=True, exist_ok=True)
    kinds, dummies, hurts, _, _ = read_cb(Path(args.cooked).expanduser())
    k0 = next(k for k in kinds if k[0] == "c0000")
    kd = dummies[k0[3]:k0[3] + k0[4]]
    dm = {(r, s): (loc, bone) for r, s, loc, model, bone in kd}
    kh = next(k for k in kinds if k[0] == "c2500")
    hh = hurts[kh[5]:kh[5] + kh[6]]

    sk = ck.Skeleton(ck.KEEP)
    hsk = ck.load_hkx_skeleton()
    c = ck.cook_clip(sk, hsk, args.clip)
    qc = ck.quantize_clip(c)
    arm = ck.reconstruct(sk, qc, c["frames"])        # (F, K, 4, 4) mirrored model space, like the runtime
    bi = {n: i for i, n in enumerate(sk.keep)}
    w0, w1 = (int(x) for x in args.window.split(","))

    def dpt(f, rid, src=1):
        loc, bone = dm[(rid, src)]
        M = arm[f, bi[bone]]
        return M[:3, :3] @ loc + M[:3, 3]

    # R1 capsule = AtkParam_Pc 2300000: dummy 100 -> 110 radius 0.4, sphere at 120 radius 0.2 (combat.json)
    rows = []
    reach = {}
    for dist in (0.8, 1.2, 1.6, 2.0, 2.4):
        hit_frames = []
        for f in range(w0, min(w1, c["frames"])):
            a, b = dpt(f, 100), dpt(f, 110)
            torso = [(h[3] + np.array([0, 0, -dist]), h[4] + np.array([0, 0, -dist]), h[2]) for h in hh]
            if any(seg_dist(a, b, p, q) <= 0.4 + r for p, q, r in torso):
                hit_frames.append(f)
        reach[dist] = hit_frames
        rows.append(f"hollow at {dist:.1f} m in front: R1 capsule touches at frames {hit_frames}")
    tip = [dpt(f, 100) for f in range(c["frames"])]
    for f in (0, w0, (w0 + w1) // 2, w1 - 1):
        rows.append(f"frame {f:3d}: sword tip (dmy 100) at {np.round(tip[f], 2).tolist()}, "
                    f"hilt (dmy 110) at {np.round(dpt(f, 110), 2).tolist()}")
    print("\n".join(rows))

    # drawing: side view (z forward = -Z to the right, y up) and top view
    S, W, H = 160, 900, 420
    img = Image.new("RGB", (W, H), (24, 24, 28))
    dr = ImageDraw.Draw(img)

    def side(p):  # x pixel from -z (forward), y pixel from height
        return (120 + (-p[2]) * S, H - 30 - p[1] * S)

    def top(p):
        return (560 + (-p[2]) * S * 0.8, H / 2 + p[0] * S * 0.8)
    for view in (side, top):
        for dist, col in ((1.2, (90, 90, 160)),):
            for p, q, r in [(h[3] + np.array([0, 0, -dist]), h[4] + np.array([0, 0, -dist]), h[2]) for h in hh]:
                dr.line([view(p), view(q)], fill=col, width=max(1, int(r * S * (0.8 if view is top else 1) * 2)))
        for f in range(w0 - 6, min(w1 + 4, c["frames"])):
            col = (255, 80, 60) if w0 <= f < w1 else (120, 110, 100)
            dr.line([view(dpt(f, 110)), view(dpt(f, 100))], fill=col, width=3 if w0 <= f < w1 else 1)
        f = (w0 + w1) // 2
        for k, n in enumerate(sk.keep):
            p = sk.kparent[k]
            if p >= 0:
                dr.line([view(arm[f, p][:3, 3]), view(arm[f, k][:3, 3])], fill=(220, 220, 220), width=2)
    dr.text((10, 8), f"{args.clip}: knight skeleton at frame {(w0 + w1)//2}, Longsword dmy100-110 frames {w0-6}..{w1+3} "
            f"(red = TAE hit window {w0}-{w1}), hollow hurt capsules at 1.2 m", fill=(230, 230, 230))
    dr.text((10, 24), "left: side view (forward ->), right: top view", fill=(180, 180, 180))
    p = out / f"{args.clip}_hitbox.png"
    img.save(p)
    print(f"image: {p}")


if __name__ == "__main__":
    main()
