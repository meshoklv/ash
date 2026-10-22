#!/usr/bin/env python3
"""Offline software render of a cooked zone pak (tools/cook_world.py) from one camera - no emulator, no lock.

    tools/.venv/bin/python tools/world_render.py <zone.pak> <out.png> "ex ey ez tx ty tz fov cell" [--lod 8] [--far 50]
        [--mode tex|id|lod] [--probe x,y ...]

Camera line = one line of cams.txt (PSP space, tools/world_cams.py); cell = collision name or "*".
Same batch selection as src/world/world.c world_draw (draw groups of the cell, chunk-box LOD, far distance, decor
distance); sky and far terrain are skipped. --mode id colours by batch, lod: near LOD green / far LOD red.
--probe x,y prints the batch under that pixel (texture source from the pak's .json next to it).
Triangles crossing the near plane are dropped (the GE clips them; good enough to tell geometry from clipping).
A diagnostic, not the renderer: no fog, no alpha test, nearest texel.
"""
from __future__ import annotations

import json
import struct
import sys
from pathlib import Path

import numpy as np
from PIL import Image

W_, H_ = 480, 272
BF_ALPHA, BF_SKY, BF_FAR, BF_MESH, BF_LOD_NEAR, BF_LOD_FAR, BF_DECOR = 1, 4, 8, 16, 64, 128, 256


def pak_entry(pak: bytes, name: str) -> bytes:
    n = struct.unpack_from("<4s3I", pak, 0)[2]
    for i in range(n):
        nm, off, size = struct.unpack_from("<16s2I", pak, 16 + 32 * i)
        if nm.rstrip(b"\0").decode() == name:
            return pak[off:off + size]
    raise KeyError(name)


def load_world(w: bytes):
    h = struct.unpack_from("<4s7I8I3f3ff", w, 0)
    n_tex, n_batch, n_vert, n_idx = h[2], h[3], h[5], h[6]
    off_tex, off_batch, off_vert, off_idx = h[8], h[9], h[11], h[12]
    chunk = h[22]
    texs = []
    for i in range(n_tex):
        tw, th, psm, fl, oc, op = struct.unpack_from("<4H2I", w, off_tex + 16 * i)
        ncl = 16 if psm == 4 else 256
        clut = np.frombuffer(w, np.uint8, ncl * 4, oc).reshape(-1, 4)
        if psm == 4:
            raw = np.frombuffer(w, np.uint8, tw * th // 2, op)
            bw = tw // 2
            blk = raw.reshape(th // 8, bw // 16, 8, 16).transpose(0, 2, 1, 3).reshape(th, bw)
            idx = np.empty((th, tw), np.uint8)
            idx[:, 0::2] = blk & 15
            idx[:, 1::2] = blk >> 4
        else:
            raw = np.frombuffer(w, np.uint8, tw * th, op)
            idx = raw.reshape(th // 8, tw // 16, 8, 16).transpose(0, 2, 1, 3).reshape(th, tw)
        texs.append(clut[idx])
    vdt = np.dtype([("u", "<u2"), ("v", "<u2"), ("c", "<u2"), ("p", "<i2", 3)])
    V = np.frombuffer(w, vdt, n_vert, off_vert)
    I = np.frombuffer(w, "<u2", n_idx, off_idx)
    batches = []
    for i in range(n_batch):
        r = struct.unpack_from("<2H4I3f3f2f2f4f4II", w, off_batch + 96 * i)
        batches.append(dict(tex=r[0], flags=r[1], vs=r[2], vc=r[3], is_=r[4], ic=r[5], off=np.array(r[6:9]),
                            sc=np.array(r[9:12]), uo=np.array(r[12:14]), us=np.array(r[14:16]),
                            center=np.array(r[16:19]), radius=r[19], mask=r[20:24], chunk=r[24]))
    return texs, batches, V, I, chunk


def cell_disp(ents: bytes, name: str):
    if name == "*":
        return (0xffffffff,) * 4
    h = struct.unpack_from("<4s7I5I", ents, 0)
    for i in range(h[2]):
        o = h[8] + 64 * i
        if ents[o:o + 16].rstrip(b"\0").decode() == name:
            return struct.unpack_from("<4I", ents, o + 16)
    raise KeyError(name)


def look_at(eye, at):
    f = at - eye; f /= np.linalg.norm(f)
    s = np.cross(f, [0, 1, 0]); s /= np.linalg.norm(s)
    u = np.cross(s, f)
    return np.stack([s, u, -f])


def main():
    a = sys.argv[1:]
    pak = Path(a[0]).expanduser().read_bytes()
    out = a[1]
    cam = a[2].split()
    opt = {a[i]: a[i + 1] for i in range(3, len(a) - 1, 2) if a[i].startswith("--")}
    probes = [tuple(int(v) for v in p.split(",")) for p in opt.get("--probe", "").split() if p]
    lod_dist, far_dist, decor_dist = float(opt.get("--lod", 8)), float(opt.get("--far", 50)), 24.0
    mode = opt.get("--mode", "tex")
    eye = np.array([float(v) for v in cam[1:4]]); at = np.array([float(v) for v in cam[4:7]])
    fov = float(cam[7]); cell = cam[8] if len(cam) > 8 else "*"
    texs, batches, V, I, chunk = load_world(pak_entry(pak, "world"))
    disp = cell_disp(pak_entry(pak, "ents"), cell)
    jpath = Path(a[0]).expanduser().with_suffix(".json")
    tnames = [str(t.get("key")) for t in json.loads(jpath.read_text())["textures"]] if jpath.exists() else []
    R = look_at(eye, at)
    fy = 1.0 / np.tan(np.radians(fov) / 2); fx = fy / (W_ / H_)
    col = np.zeros((H_, W_, 3), np.uint8); col[:] = (48, 53, 58)
    zb = np.full((H_, W_), np.inf); idb = np.full((H_, W_), -1, np.int32)
    rng = np.random.default_rng(3)
    pal = rng.integers(40, 255, (len(batches), 3)).astype(np.uint8)
    hc = chunk / 2
    for bi, b in enumerate(batches):
        fl = b["flags"]
        if fl & (BF_SKY | BF_MESH | BF_FAR):
            continue
        if not any(m & d for m, d in zip(b["mask"], disp)):
            continue
        if fl & (BF_LOD_NEAR | BF_LOD_FAR):
            ix = (b["chunk"] & 1023) - 512; iy = ((b["chunk"] >> 10) & 1023) - 512; iz = ((b["chunk"] >> 20) & 1023) - 512
            c = (np.array([ix, iy, iz]) + 0.5) * chunk
            dd = np.maximum(np.abs(c - eye) - hc, 0)
            near = (dd @ dd) < lod_dist ** 2
            if near != bool(fl & BF_LOD_NEAR):
                continue
        dc = b["center"] - eye
        lim = (decor_dist if fl & BF_DECOR else far_dist) + b["radius"]
        if dc @ dc > lim * lim:
            continue
        vv = V[b["vs"]:b["vs"] + b["vc"]]
        P = b["off"] + vv["p"].astype(np.float64) / 32768 * b["sc"]
        UV = b["uo"] + np.stack([vv["u"], vv["v"]], 1).astype(np.float64) / 32768 * b["us"]
        c565 = vv["c"].astype(np.int32)
        VC = np.stack([(c565 & 31) / 31, ((c565 >> 5) & 63) / 63, ((c565 >> 11) & 31) / 31], 1)
        T = I[b["is_"]:b["is_"] + b["ic"]].reshape(-1, 3).astype(np.int64)
        Pv = (P - eye) @ R.T                    # x right, y up, z backwards (camera looks down -z)
        z = -Pv[:, 2]
        sx = (Pv[:, 0] / np.where(z > 1e-6, z, 1e-6) * fx * 0.5 + 0.5) * W_
        sy = (0.5 - Pv[:, 1] / np.where(z > 1e-6, z, 1e-6) * fy * 0.5) * H_
        tex = texs[b["tex"]]
        th, tw = tex.shape[:2]
        for t in T:
            zt = z[t]
            if (zt < 0.3).any():
                continue
            xs, ys = sx[t], sy[t]
            x0, x1 = max(0, int(np.floor(xs.min()))), min(W_ - 1, int(np.ceil(xs.max())))
            y0, y1 = max(0, int(np.floor(ys.min()))), min(H_ - 1, int(np.ceil(ys.max())))
            if x0 > x1 or y0 > y1:
                continue
            area = (xs[1] - xs[0]) * (ys[2] - ys[0]) - (xs[2] - xs[0]) * (ys[1] - ys[0])
            if abs(area) < 1e-9:
                continue
            if not (fl & 2) and area > 0:   # CCW front faces in GE screen space (y down) -> negative area here
                continue
            gx, gy = np.meshgrid(np.arange(x0, x1 + 1) + 0.5, np.arange(y0, y1 + 1) + 0.5)
            w0 = ((xs[1] - gx) * (ys[2] - gy) - (xs[2] - gx) * (ys[1] - gy)) / area
            w1 = ((xs[2] - gx) * (ys[0] - gy) - (xs[0] - gx) * (ys[2] - gy)) / area
            w2 = 1 - w0 - w1
            ins = (w0 >= 0) & (w1 >= 0) & (w2 >= 0)
            if not ins.any():
                continue
            iz = w0 / zt[0] + w1 / zt[1] + w2 / zt[2]
            zz = 1 / iz
            sub = zb[y0:y1 + 1, x0:x1 + 1]
            m = ins & (zz < sub)
            if not m.any():
                continue
            sub[m] = zz[m]
            idb[y0:y1 + 1, x0:x1 + 1][m] = bi
            if mode == "id":
                col[y0:y1 + 1, x0:x1 + 1][m] = pal[bi]
            elif mode == "lod":
                col[y0:y1 + 1, x0:x1 + 1][m] = (60, 200, 60) if fl & BF_LOD_NEAR else ((220, 60, 60) if fl & BF_LOD_FAR else (200, 200, 60))
            else:
                k = np.stack([w0 / zt[0], w1 / zt[1], w2 / zt[2]], -1) * zz[..., None]
                uv = k[m] @ UV[t]
                vc = k[m] @ VC[t]
                tx = (np.floor(uv[:, 0] * tw).astype(np.int64)) % tw
                ty = (np.floor(uv[:, 1] * th).astype(np.int64)) % th
                rgb = tex[ty, tx, :3].astype(np.float64) * vc * 2.0
                col[y0:y1 + 1, x0:x1 + 1][m] = np.clip(rgb, 0, 255).astype(np.uint8)
    Image.fromarray(col).save(out)
    for (px, py) in probes:
        bi = idb[py, px]
        if bi < 0:
            print(f"probe {px},{py}: nothing"); continue
        b = batches[bi]
        print(f"probe {px},{py}: batch {bi} tex {b['tex']} {tnames[b['tex']] if b['tex'] < len(tnames) else ''} "
              f"flags {b['flags']} tris {b['ic'] // 3} chunk {b['chunk']:#x} z {zb[py, px]:.2f} m")


if __name__ == "__main__":
    main()
