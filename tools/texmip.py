"""Mip chains for CLUT textures (T4 / T8) -> `.ashmip` sidecar for an `.ashm` scene (docs/render.md §2).

Why: the cooked scenes have 128x128 CLUT8 textures and no mip levels, so distant floors/walls alias ("shimmer")
when the camera moves. The GE samples up to 8 levels (sceGuTexMode maxmips + sceGuTexImage(level) +
sceGuTexLevelMode); src/render/render.c binds them (render_tex_bind).

How a level is made: palette -> RGBA float, 2x2 box filter (alpha-aware: colour averaged over the opaque texels,
alpha = coverage >= 0.5, the same binary alpha the cooker uses), then mapped back to the SAME CLUT (nearest
palette colour among opaque entries, KD-tree in RGB). The CLUT is shared by all levels, so one sceGuClutLoad
serves the whole chain. Levels stop at 16 texels wide (T8: 16 B swizzle block) / 8 rows (swizzle block height)
or at 8 levels total.

Library use (other cookers, e.g. tools/cook_world.py):
    from texmip import mip_levels, swizzle
    levels = mip_levels(idx_hw_uint8, clut_rgba_n4, psm=5)   # [level1, level2, ...] as (h, w) index arrays
    blobs  = [swizzle(l, psm) for l in levels]                # bytes per level, GE-swizzled

CLI (sidecar for an existing scene, nothing in the .ashm changes):
    tools/.venv/bin/python tools/texmip.py ~/ash-assets/cooked/arena_nochars.ashm      # -> arena_nochars.ashmip
    tools/.venv/bin/python tools/texmip.py ~/ash-assets/cooked/world/zone_m18_01.pak   # zone package: WorldTex order

AMIP v1 (little-endian, 16-aligned):
    Header 16 B : "AMIP", u32 version=1, u32 n_tex (= .ashm n_tex), u32 pad
    Entry  64 B : u16 n_levels (extra levels 1..n, 0..7), u16 psm (4 = T4, 5 = T8),
                  u16 w[8], u16 h[8]  (index 0 = level 0 of the .ashm texture, then levels 1..n),
                  u32 off[7]          (levels 1..7, from file start, GE-swizzled, same CLUT as level 0)
"""
from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

import numpy as np
from scipy.spatial import cKDTree

MAGIC = b"AMIP"
VERSION = 1
HDR_FMT = "<4s3I"           # 16 B
ENTRY_FMT = "<HH8H8H7I"     # 4 + 16 + 16 + 28 = 64 B
ENTRY_SIZE = 64
assert struct.calcsize(ENTRY_FMT) == ENTRY_SIZE

PSM_T4, PSM_T8 = 4, 5


# ------------------------------------------------------------------ swizzle

def swizzle(pix: np.ndarray, psm: int) -> bytes:
    """GE swizzle: blocks of 16 bytes x 8 rows. pix = (h, w) palette indices."""
    h, w = pix.shape
    if psm == PSM_T4:
        lo = pix[:, 0::2].astype(np.uint8) & 15
        hi = pix[:, 1::2].astype(np.uint8) & 15
        b = (lo | (hi << 4)).astype(np.uint8)       # low nibble = first texel
    else:
        b = pix.astype(np.uint8)
    hb, wb = b.shape
    assert wb % 16 == 0 and hb % 8 == 0, (w, h, psm)
    blk = b.reshape(hb // 8, 8, wb // 16, 16).transpose(0, 2, 1, 3)
    return np.ascontiguousarray(blk).tobytes()


def unswizzle(data: bytes, w: int, h: int, psm: int) -> np.ndarray:
    """Inverse of swizzle -> (h, w) palette indices."""
    wb = w // 2 if psm == PSM_T4 else w
    b = np.frombuffer(data, np.uint8, count=wb * h)
    b = b.reshape(h // 8, wb // 16, 8, 16).transpose(0, 2, 1, 3).reshape(h, wb)
    if psm == PSM_T4:
        out = np.empty((h, w), np.uint8)
        out[:, 0::2] = b & 15
        out[:, 1::2] = b >> 4
        return out
    return b.copy()


# ------------------------------------------------------------------ levels

def level_ok(w: int, h: int, psm: int) -> bool:
    min_w = 32 if psm == PSM_T4 else 16      # 16 bytes per swizzle row
    return w >= min_w and h >= 8


def mip_levels(idx: np.ndarray, clut: np.ndarray, psm: int = PSM_T8, max_levels: int = 7) -> list[np.ndarray]:
    """Extra mip levels (level 1..n) of a CLUT texture, each mapped back to the same CLUT.

    idx  : (h, w) uint8 palette indices (unswizzled), level 0
    clut : (n, 4) uint8 RGBA (n = 16 or 256)
    """
    clut = np.asarray(clut, np.uint8).reshape(-1, 4)
    pal = clut.astype(np.float32) / 255.0
    opaque = np.nonzero(clut[:, 3] >= 128)[0]
    clear = np.nonzero(clut[:, 3] < 128)[0]
    if len(opaque) == 0:
        return []
    # perceptual-ish weights so dark greys do not snap to tinted entries
    wgt = np.array([0.30, 0.59, 0.11], np.float32) ** 0.5
    tree = cKDTree(pal[opaque, :3] * wgt)
    rgba = pal[idx]                                   # (h, w, 4)
    out = []
    w, h = idx.shape[1], idx.shape[0]
    while len(out) < max_levels:
        nw, nh = w // 2, h // 2
        if nw < 1 or nh < 1 or not level_ok(nw, nh, psm):
            break
        q = rgba.reshape(nh, 2, nw, 2, 4)
        a = (q[..., 3] >= 0.5).astype(np.float32)     # binary alpha of the source texels
        cov = a.mean(axis=(1, 3))                     # (nh, nw)
        col = (q[..., :3] * a[..., None]).sum(axis=(1, 3)) / np.maximum(a.sum(axis=(1, 3)), 1e-6)[..., None]
        # fully transparent blocks: plain average (colour does not matter, alpha decides)
        col = np.where(cov[..., None] > 0, col, q[..., :3].mean(axis=(1, 3)))
        _, near = tree.query((col * wgt).reshape(-1, 3))
        lvl = opaque[near].reshape(nh, nw).astype(np.uint8)
        if len(clear):
            lvl[cov < 0.5] = clear[0]
        out.append(lvl)
        rgba = np.concatenate([col, (cov >= 0.5).astype(np.float32)[..., None]], -1)
        w, h = nw, nh
    return out


# ------------------------------------------------------------------ .ashm sidecar

def read_ashm_textures(path: Path):
    """-> list of (w, h, psm, clut (n,4) uint8, idx (h,w) uint8) from an .ashm v1 (tools/ashm.py layout)."""
    data = path.read_bytes()
    magic, version, n_tex, n_batch, n_vert, n_idx, n_cam, flags, off_tex = struct.unpack_from("<4s8I", data, 0)
    assert magic == b"ASHM" and version == 1, (magic, version)
    out = []
    for i in range(n_tex):
        w, h, psm, tflags, off_clut, off_pix = struct.unpack_from("<4H2I", data, off_tex + 16 * i)
        n = 16 if psm == PSM_T4 else 256
        clut = np.frombuffer(data, np.uint8, count=n * 4, offset=off_clut).reshape(n, 4)
        size = (w // 2 if psm == PSM_T4 else w) * h
        idx = unswizzle(data[off_pix:off_pix + size], w, h, psm)
        out.append((w, h, psm, clut, idx))
    return out


def read_pak_textures(path: Path):
    """Same for the world textures of a zone package (tools/ashw.py: APAK -> "world" .ashw, WorldTex order)."""
    data = path.read_bytes()
    magic, _ver, n = struct.unpack_from("<4s2I", data, 0)
    assert magic == b"APAK", magic
    ents = {struct.unpack_from("<16s", data, 16 + 32 * i)[0].split(b"\0")[0].decode():
            struct.unpack_from("<2I", data, 16 + 32 * i + 16) for i in range(n)}
    off, _ = ents["world"]
    n_tex = struct.unpack_from("<I", data, off + 8)[0]
    off_tex = struct.unpack_from("<I", data, off + 32)[0]
    out = []
    for i in range(n_tex):
        w, h, psm, _fl, oc, op = struct.unpack_from("<4H2I", data, off + off_tex + 16 * i)
        nc = 16 if psm == PSM_T4 else 256
        clut = np.frombuffer(data, np.uint8, count=nc * 4, offset=off + oc).reshape(nc, 4)
        size = (w // 2 if psm == PSM_T4 else w) * h
        out.append((w, h, psm, clut, unswizzle(data[off + op:off + op + size], w, h, psm)))
    return out


def write_ashmip(path: Path, texs) -> dict:
    """texs = list of (w, h, psm, levels[list of (h,w) arrays]). Returns stats."""
    n = len(texs)
    buf = bytearray(struct.pack(HDR_FMT, MAGIC, VERSION, n, 0))
    ent_off = len(buf)
    buf += bytes(ENTRY_SIZE * n)
    stats = {"tex": n, "levels": 0, "bytes_levels": 0}
    for i, (w, h, psm, levels) in enumerate(texs):
        ws, hs, offs = [0] * 8, [0] * 8, [0] * 7
        ws[0], hs[0] = w, h
        for k, lv in enumerate(levels[:7]):
            while len(buf) % 16:
                buf.append(0)
            offs[k] = len(buf)
            blob = swizzle(lv, psm)
            buf += blob
            hs[k + 1], ws[k + 1] = lv.shape
            stats["levels"] += 1
            stats["bytes_levels"] += len(blob)
        struct.pack_into(ENTRY_FMT, buf, ent_off + ENTRY_SIZE * i, len(levels[:7]), psm, *ws, *hs, *offs)
    while len(buf) % 16:
        buf.append(0)
    path.write_bytes(bytes(buf))
    stats["bytes"] = len(buf)
    return stats


def preview(path: Path, texs_src, texs_lv, k: int = 0):
    """Debug PNG strip of texture k: level 0..n side by side (outside git, next to the sidecar)."""
    from PIL import Image
    w, h, psm, clut, idx = texs_src[k]
    ims = [clut[idx]] + [clut[l] for l in texs_lv[k]]
    W = sum(i.shape[1] for i in ims)
    canvas = np.zeros((h, W, 4), np.uint8)
    x = 0
    for im in ims:
        canvas[:im.shape[0], x:x + im.shape[1]] = im
        x += im.shape[1]
    Image.fromarray(canvas, "RGBA").save(path)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("ashm", type=Path)
    ap.add_argument("-o", "--out", type=Path, help="default: <ashm stem>.ashmip next to it")
    ap.add_argument("--levels", type=int, default=7, help="max extra levels (GE: up to 7 besides level 0)")
    ap.add_argument("--preview", type=int, default=-1, help="write <out>.tex<k>.png strip of texture k")
    a = ap.parse_args(argv)
    out = a.out or a.ashm.with_suffix(".ashmip")
    src = read_pak_textures(a.ashm) if a.ashm.suffix == ".pak" else read_ashm_textures(a.ashm)
    lv = [mip_levels(idx, clut, psm, a.levels) for (w, h, psm, clut, idx) in src]
    st = write_ashmip(out, [(w, h, psm, l) for (w, h, psm, _, _), l in zip(src, lv)])
    level0 = sum((w // 2 if psm == PSM_T4 else w) * h for (w, h, psm, _, _) in src)
    print(f"texmip: {a.ashm.name}: {st['tex']} textures, {st['levels']} extra levels, "
          f"+{st['bytes_levels'] / 1024:.1f} KB on top of {level0 / 1024:.1f} KB level 0 "
          f"(+{100 * st['bytes_levels'] / max(level0, 1):.0f}%) -> {out} ({st['bytes'] / 1024:.1f} KB)")
    if a.preview >= 0:
        preview(out.with_suffix(f".tex{a.preview}.png"), src, lv, a.preview)
    return 0


if __name__ == "__main__":
    sys.exit(main())
