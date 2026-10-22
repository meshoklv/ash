"""Second light pass + zone look for an `.ashm` scene -> `.ashl` (docs/render.md §3-5).

What it bakes (all from the user's DSR copy, nothing goes to git):
  * point lights of the map: MSB `lights` events (torches, gate light...) with their PointLightBank row
    (fade start/end, colour, strength) + MSB `vfx` torches (vfx 530) that have no light event;
  * a top-down LIGHTMAP (CLUT8, <=128x128) of the warm light pools those lights throw on the floor, with shadows
    (rays against the Havok collision `.pcol`), drawn at runtime as a second ADDITIVE pass (texel 0 = no change)
    through GE texture projection from the vertex position
    (sceGuTexMapMode(GU_TEXTURE_MATRIX) + sceGuTexProjMapMode(GU_POSITION)) - no second UV set, no extra vertices;
  * the TRIANGLE SUBSET that the second pass draws: only triangles near a light (horizontal radius + a vertical
    window around the light), so the pass costs a fraction of the scene, and lights do not leak to other floors;
  * zone look from DrawParam a<area>: fog (FogBank row of the floor collision's MSB fog_id) and character light
    (LightBank row of its ambient_light_id: 3 directional lights + top/bottom ambient), converted to GE terms.
  * optional `--vertex-out`: the same lights ADDED to the scene's vertex colours (a copy of the .ashm; the GE
    multiplies them by the texture, so this is real albedo x light), zero runtime cost, resolution = vertex density.

    tools/.venv/bin/python tools/cook_light.py --scene ~/ash-assets/cooked/arena_nochars.ashm \
        --pcol ~/ash-assets/cooked/arena.pcol --floor h0005B1 --out ~/ash-assets/cooked/arena.ashl

Library use (tools/cook_world.py etc.):
    lights = msb_lights("m18_01_00_00", "a18")           # list[Light] in PSP space
    fog    = zone_fog("m18_01_00_00", "a18", "h0005B1")   # dict(near, far, color, src)
    chl    = zone_char_light(...)                          # dict(dirs, colors, ambient, src)
    fog_table("m18_01_00_00", "a18")                       # {collision name: fog dict} for per-cell fog

ASHL v1 (little-endian, 16-aligned, offsets from file start)
    Header 160 B: "ASHL", u32 version=1, n_light, n_sub, n_idx, lm_w, lm_h, flags
                  u32 off_light, off_sub, off_idx, off_clut, off_pix, pad[3]
                  f32 lm_x0, lm_z0, lm_sx, lm_sz        u = (x - x0) / sx, v = (z - z0) / sz   (PSP space)
                  f32 fog_near, fog_far; u32 fog_color (ABGR); u32 clear_color
                  f32 chl_dir[3][3] (unit, towards the light), u32 chl_col[3] (ABGR), u32 chl_ambient, f32 lm_gain
                  pad to 160
    Light 32 B  : f32 pos[3], radius; u32 color (ABGR); u16 kind (0 torch, 1 bonfire, 2 other), u16 flags;
                  f32 strength, phase
    Sub   16 B  : u32 batch (index in the .ashm), idx_start (in this file's index array), idx_count, pad
    Index  2 B  : u16, relative to the .ashm batch's vert_start (same as .ashm indices)
    CLUT  1 KB  : 256 x ABGR8888 (sorted by brightness, 0 = black; runtime scales c for flicker)
    Pixels      : lm_w * lm_h bytes, GE-swizzled
"""
from __future__ import annotations

import argparse
import json
import logging
import math
import re
import struct
import sys
from dataclasses import dataclass
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from texmip import swizzle  # noqa: E402

logging.getLogger("soulstruct").setLevel(logging.ERROR)

MAGIC = b"ASHL"
VERSION = 1
HDR_SIZE = 160
TORCH_VFX = {530}                    # m18_01 torch flame vfx [проверено: MSB vfx events 松明_xx]
CHL_GAIN = 0.48                      # strongest LightBank light -> GE colour 0.62 [придумал, на глаз]
RADIUS_SCALE = 1.6                   # DS1 point lights are tuned for characters (2-5 m); pools on the map
                                     # look right a bit wider [придумал, на глаз]
STRENGTH_SCALE = 0.9                 # lightmap gain on top of PointLightBank alpha/100 [придумал, на глаз]
V_WINDOW = (-7.0, 4.0)               # triangles lit by a light: y within light.y + window [придумал]
MIN_GAIN = 0.06                      # second pass skips triangles whose light adds < MIN_GAIN*ADD_GAIN [придумал]
ADD_GAIN = 0.18                      # additive lightmap: light 1.0 -> +46/255 [придумал, на глаз]
VERT_GAIN = 0.40                     # vertex bake: colour += light * 255 * VERT_GAIN [придумал, на глаз]


def dsr_root() -> Path:
    from dsrlib import DSR
    return DSR


# ------------------------------------------------------------------ DSR data

@dataclass
class Light:
    name: str
    pos: np.ndarray        # PSP space (x mirrored)
    radius: float          # fade end, scaled
    inner: float           # fade start, scaled
    color: np.ndarray      # 0..1 RGB
    strength: float
    kind: int = 0
    row: int = -1


def _draw_params(area: str):
    """{bank name: {row id: row}} of a<area>_DrawParam slot 0 (Soulstruct; entry names carry full paths, so the
    name regex is widened here)."""
    import soulstruct.darksouls1ptde.params.draw_param.drawparambnd as m
    from soulstruct.darksouls1r.params.draw_param import DrawParamBND
    m._DRAW_PARAM_FILE_NAME_RE = re.compile(r".*?([ms]\d\d|default)(_\d)?(_\w+)\.param")
    logging.disable(logging.WARNING)
    try:
        b = DrawParamBND.from_path(dsr_root() / "param" / "DrawParam" / f"{area}_DrawParam.parambnd.dcx")
        for e in b.entries:
            b.load_from_entry(e)
    finally:
        logging.disable(logging.NOTSET)
    return {k: dict(v.rows) for k, v in b.draw_params_0.items() if v is not None}


def _msb(map_id: str):
    from soulstruct.darksouls1r.maps import MSB
    return MSB.from_path(dsr_root() / "map" / "MapStudio" / f"{map_id}.msb")


def _psp(p) -> np.ndarray:
    return np.array([-float(p[0]), float(p[1]), float(p[2])], np.float32)


def msb_lights(map_id: str, area: str) -> list[Light]:
    msb = _msb(map_id)
    dp = _draw_params(area)
    pl = dp.get("PointLightBank", {})
    out: list[Light] = []
    seen = []
    for e in msb.lights:
        reg = getattr(e, "attached_region", None)
        if reg is None or e.point_light_id < 0 or e.point_light_id not in pl:
            continue
        r = pl[e.point_light_id]
        pos = _psp(reg.translate)
        if any(np.linalg.norm(pos - s) < 0.3 for s in seen):
            continue                                  # MSB has duplicate events for some torches
        seen.append(pos)
        col = np.array([r.PointLightRed, r.PointLightGreen, r.PointLightBlue], np.float32) / 255.0
        out.append(Light(e.name, pos, float(r.FadeEndDistance) * RADIUS_SCALE,
                         float(r.FadeStartDistance) * RADIUS_SCALE, col, float(r.PointLightAlpha) / 100.0,
                         0 if "松明" in e.name else 2, e.point_light_id))
    # torches with a flame vfx but no light event: same light as a small torch (PointLightBank 15) [версия]
    small = pl.get(15)
    for e in msb.vfx:
        reg = getattr(e, "attached_region", None)
        if reg is None or e.vfx_id not in TORCH_VFX:
            continue
        pos = _psp(reg.translate)
        if any(np.linalg.norm(pos - s) < 0.3 for s in seen):
            continue
        seen.append(pos)
        if small is None:
            continue
        col = np.array([small.PointLightRed, small.PointLightGreen, small.PointLightBlue], np.float32) / 255.0
        out.append(Light(e.name + "(vfx)", pos, float(small.FadeEndDistance) * RADIUS_SCALE,
                         float(small.FadeStartDistance) * RADIUS_SCALE, col, float(small.PointLightAlpha) / 100.0,
                         0, 15))
    return out


def _abgr(rgb, a=255) -> int:
    r, g, b = [int(max(0, min(255, round(float(c))))) for c in rgb]
    return (a << 24) | (b << 16) | (g << 8) | r


def _collision(map_id: str, name: str):
    for c in _msb(map_id).collisions:
        if c.name == name:
            return c
    raise SystemExit(f"collision {name} not in {map_id}")


def fog_from_row(r, src: str) -> dict:
    """DS1 FogBank row -> GE linear fog [версия: DS1 fog = colour blended up to alpha/255 between start and end;
    GE fog reaches 100 % at `far`, so far is stretched by 1/alpha and the colour darkened for the PSP's LDR]."""
    a = max(float(r.Alpha) / 255.0, 0.05)
    start, end = float(r.FogStartDistance), float(r.FogEndDistance)
    near = start
    far = min(start + (end - start) / a, 160.0)
    far = max(far, near + 20.0)
    ds = np.array([r.Red, r.Green, r.Blue], np.float32)
    color = ds * 0.32                                  # [придумал, на глаз] DS1 HDR haze -> LDR dark hall
    return dict(near=near, far=far, color=_abgr(color), clear=_abgr(color * 0.45), src=src,
                ds=dict(start=start, end=end, rgb=[int(r.Red), int(r.Green), int(r.Blue)], alpha=int(r.Alpha)))


def zone_fog(map_id: str, area: str, floor: str) -> dict:
    c = _collision(map_id, floor)
    fb = _draw_params(area).get("FogBank", {})
    if c.fog_id not in fb:
        return dict(near=30.0, far=110.0, color=0xff1a1410, clear=0xff1a1410, src="default (no FogBank row)")
    return fog_from_row(fb[c.fog_id], f"{area} FogBank[{c.fog_id}] '{fb[c.fog_id].Name}' via {floor}.fog_id")


def fog_table(map_id: str, area: str) -> dict:
    fb = _draw_params(area).get("FogBank", {})
    return {c.name: fog_from_row(fb[c.fog_id], f"FogBank[{c.fog_id}]") for c in _msb(map_id).collisions
            if c.fog_id in fb}


def zone_char_light(map_id: str, area: str, floor: str) -> dict:
    """LightBank row (MSB ambient_light_id of the floor collision): 3 'baked' directional lights + top-down /
    bottom-up ambient -> GE lights 0-2 + ambient [версия: RotationX = elevation of the light source in degrees,
    RotationY = yaw; alpha/100 = strength; scaled so the strongest light is CHL_GAIN]."""
    c = _collision(map_id, floor)
    lb = _draw_params(area).get("LightBank", {})
    r = lb.get(c.ambient_light_id)
    if r is None:
        return dict(dirs=[[0.35, 0.8, 0.45], [-0.5, 0.3, -0.6], [0, -1, 0]], colors=[0xffd9b286, 0xff604838, 0],
                    ambient=0xff4a4a4a, src="default")
    dirs, cols, st = [], [], []
    for k in range(3):
        el = math.radians(getattr(r, f"BakedLight{k}RotationX"))
        yaw = math.radians(getattr(r, f"BakedLight{k}RotationY"))
        d = np.array([-math.sin(yaw) * math.cos(el), math.sin(el), math.cos(yaw) * math.cos(el)])   # x mirrored
        dirs.append((d / np.linalg.norm(d)).round(4).tolist())
        rgb = np.array([getattr(r, f"BakedLight{k}{ch}") for ch in ("Red", "Green", "Blue")], np.float32)
        st.append(getattr(r, f"BakedLight{k}Alpha") / 100.0)
        cols.append(rgb)
    m = max(max(st), 1e-3)
    # lights weaker than 30 % of the strongest are dropped (colour 0 -> the GE light stays off): each GE light
    # costs per skinned vertex (PSP-est: 4 lights on knight + demon = +2.2 ms) [придумал]
    colors = [_abgr(cols[k] * (st[k] / m) * CHL_GAIN) if st[k] >= 0.3 * m else 0 for k in range(3)]
    td = np.array([r.TopDownLightRed, r.TopDownLightGreen, r.TopDownLightBlue], np.float32) * r.TopDownLightAlpha / 100
    bu = np.array([r.BottomUpLightRed, r.BottomUpLightGreen, r.BottomUpLightBlue], np.float32) * r.BottomUpLightAlpha / 100
    amb = np.clip((td + bu) * 0.5 * 0.25 + 20, 0, 255)                # [придумал] floor so backs never go black
    return dict(dirs=dirs, colors=colors, ambient=_abgr(amb),
                src=f"{area} LightBank[{c.ambient_light_id}] '{r.Name}' via {floor}.ambient_light_id",
                strength=st)


# ------------------------------------------------------------------ cooked files

def read_ashm(path: Path):
    data = bytearray(path.read_bytes())
    (magic, version, n_tex, n_batch, n_vert, n_idx, n_cam, flags, off_tex, off_batch, off_vert, off_idx,
     off_cam) = struct.unpack_from("<4s12I", data, 0)
    assert magic == b"ASHM" and version == 1
    batches = []
    for i in range(n_batch):
        tex, fl, vs, vc, ist, ic, cx, cy, cz, rad = struct.unpack_from("<2H4I4f", data, off_batch + 36 * i)
        batches.append(dict(tex=tex, flags=fl, vs=vs, vc=vc, ist=ist, ic=ic, c=(cx, cy, cz), r=rad))
    vdt = np.dtype([("uv", "<f4", 2), ("color", "<u4"), ("pos", "<f4", 3)])
    vert = np.frombuffer(data, vdt, count=n_vert, offset=off_vert)
    idx = np.frombuffer(data, "<u2", count=n_idx, offset=off_idx)
    return dict(data=data, batches=batches, vert=vert, idx=idx, off_vert=off_vert)


def read_pcol(path: Path):
    d = path.read_bytes()
    magic, ver, n_vert, n_tri, n_part, nx, nz, n_list = struct.unpack_from("<4s7I", d, 0)
    assert magic == b"PCOL"
    off_vert, off_tri, off_part = struct.unpack_from("<3I", d, 48)
    V = np.frombuffer(d, "<f4", count=n_vert * 3, offset=off_vert).reshape(-1, 3)
    tdt = np.dtype([("v", "<u2", 3), ("mat", "<u2"), ("part", "u1"), ("flags", "u1")])
    T = np.frombuffer(d, tdt, count=n_tri, offset=off_tri)
    parts = [struct.unpack_from("<16s", d, off_part + 24 * i)[0].split(b"\0")[0].decode() for i in range(n_part)]
    return V[T["v"].astype(np.int64)], T["part"], parts          # (n_tri, 3, 3)


# ------------------------------------------------------------------ rays

def ray_hits(orig: np.ndarray, dirs: np.ndarray, tris: np.ndarray, tmax: np.ndarray, chunk=256):
    """Möller-Trumbore, numpy. orig/dirs (R,3), tris (T,3,3), tmax (R,). -> (nearest t (inf = none), tri index)."""
    best_t = np.full(len(orig), np.inf, np.float32)
    best_i = np.full(len(orig), -1, np.int64)
    if len(tris) == 0:
        return best_t, best_i
    v0 = tris[:, 0]
    e1 = tris[:, 1] - v0
    e2 = tris[:, 2] - v0
    for s in range(0, len(orig), chunk):
        o = orig[s:s + chunk, None, :]
        d = dirs[s:s + chunk, None, :]
        p = np.cross(d, e2[None])
        det = (e1[None] * p).sum(-1)
        ok = np.abs(det) > 1e-9
        inv = np.where(ok, 1.0 / np.where(ok, det, 1), 0)
        tv = o - v0[None]
        u = (tv * p).sum(-1) * inv
        q = np.cross(tv, e1[None])
        v = (d * q).sum(-1) * inv
        t = (e2[None] * q).sum(-1) * inv
        hit = ok & (u >= 0) & (v >= 0) & (u + v <= 1) & (t > 1e-3) & (t < tmax[s:s + chunk, None])
        t = np.where(hit, t, np.inf)
        i = t.argmin(1)
        best_t[s:s + chunk] = t[np.arange(len(i)), i]
        best_i[s:s + chunk] = np.where(np.isfinite(best_t[s:s + chunk]), i, -1)
    return best_t, best_i


def near_tris(tris, centre, r):
    c = tris.mean(1)
    rad = np.linalg.norm(tris - c[:, None], axis=2).max(1)
    return tris[np.linalg.norm(c - centre, axis=1) < r + rad]


def light_at(points: np.ndarray, normals: np.ndarray | None, lights: list[Light], col_tris) -> np.ndarray:
    """Sum of the lights at points (N,3) with shadow rays against the collision. -> (N,3) linear 0..~2."""
    out = np.zeros((len(points), 3), np.float32)
    for L in lights:
        d = np.linalg.norm(points - L.pos, axis=1)
        m = d < L.radius
        if not m.any():
            continue
        P = points[m]
        to = L.pos - P
        dist = np.linalg.norm(to, axis=1)
        dirn = to / np.maximum(dist, 1e-4)[:, None]
        tris = near_tris(col_tris, L.pos, L.radius + 1.0)
        # start a bit off the surface, stop short of the torch (it is mounted in a wall)
        t, _ = ray_hits(P + dirn * 0.15, dirn, tris, np.maximum(dist - 0.45, 0.0))
        vis = ~np.isfinite(t)
        x = np.clip((dist - L.inner) / max(L.radius - L.inner, 1e-3), 0, 1)
        att = (1 - x * x * (3 - 2 * x))                           # smoothstep fade start -> end
        att = att * (0.35 + 0.65 * np.clip(1.0 - dist / L.radius, 0, 1))
        if normals is not None:
            ndl = (normals[m] * dirn).sum(1)
            att = att * np.clip(0.25 + 0.75 * ndl, 0, 1)
        out[m] += (L.color[None] * min(L.strength, 2.2) * STRENGTH_SCALE) * (att * vis)[:, None]
    return out


# ------------------------------------------------------------------ bake

def bake(scene: Path, pcol: Path, lights: list[Light], lm_max: int = 128, max_pass_tris: int = 2500):
    sc = read_ashm(scene)
    col_tris, _, _ = read_pcol(pcol)
    P = sc["vert"]["pos"].astype(np.float32)
    lo, hi = P.min(0), P.max(0)
    lights = [L for L in lights if np.all(L.pos >= lo - 2) and np.all(L.pos <= hi + 2)]
    if not lights:
        raise SystemExit("no lights inside the scene bbox")
    # lightmap rect = union of the light discs (XZ), clipped to the scene
    xs = [L.pos[0] - L.radius for L in lights] + [L.pos[0] + L.radius for L in lights]
    zs = [L.pos[2] - L.radius for L in lights] + [L.pos[2] + L.radius for L in lights]
    x0, x1 = max(min(xs), lo[0]), min(max(xs), hi[0])
    z0, z1 = max(min(zs), lo[2]), min(max(zs), hi[2])
    ext = max(x1 - x0, z1 - z0)
    texel = max(ext / lm_max, 0.12)
    w = 1 << max(4, math.ceil(math.log2(max((x1 - x0) / texel, 16))))
    h = 1 << max(3, math.ceil(math.log2(max((z1 - z0) / texel, 8))))
    w, h = min(w, lm_max), min(h, lm_max)
    sx, sz = w * texel, h * texel
    x0 -= (sx - (x1 - x0)) / 2
    z0 -= (sz - (z1 - z0)) / 2
    # per texel and light: receiver = first surface under the texel below the light (floor of that level)
    gx, gz = np.meshgrid(x0 + (np.arange(w) + 0.5) * texel, z0 + (np.arange(h) + 0.5) * texel)
    acc = np.zeros((h, w, 3), np.float32)
    for L in lights:
        m = np.hypot(gx - L.pos[0], gz - L.pos[2]) < L.radius
        if not m.any():
            continue
        top = L.pos[1] + 0.6
        orig = np.stack([gx[m], np.full(m.sum(), top), gz[m]], 1).astype(np.float32)
        dn = np.tile(np.array([[0, -1, 0]], np.float32), (len(orig), 1))
        tris = near_tris(col_tris, L.pos, L.radius + 2.0)
        t, ti = ray_hits(orig, dn, tris, np.full(len(orig), 10.0, np.float32))
        ok = np.isfinite(t)
        rec = orig.copy()
        rec[:, 1] = np.where(ok, top - t, L.pos[1] - 1.6)          # no floor: assume a wall near the torch
        acc[m] += light_at(rec + np.array([0, 0.05, 0], np.float32), None, [L], col_tris)
    # additive light: texel = light * ADD_GAIN (black = no change). A multiply pass cannot light the near-black
    # baked floors of the Asylum (raw values 8-16/255 next to the torches [проверено дампом кадра]).
    rgb = np.clip(acc * ADD_GAIN * 255.0, 0, 255)
    # CLUT: 256 entries sorted by brightness (index 0 = black = no change)
    flat = rgb.reshape(-1, 3)
    from PIL import Image
    im = Image.fromarray(flat.reshape(h, w, 3).astype(np.uint8))
    q = im.quantize(colors=255, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)
    pal = np.array(q.getpalette()[:255 * 3], np.uint8).reshape(-1, 3)
    order = np.argsort(pal.astype(np.int32).sum(1))
    pal = pal[order]
    remap = np.empty(len(order), np.int64)
    remap[order] = np.arange(len(order))
    idx = remap[np.asarray(q, np.int64)] + 1
    # pin neutral: texels with no light -> entry 0 = black
    idx[acc.sum(2) < 1e-4] = 0
    clut = np.zeros((256, 4), np.uint8)
    clut[0] = (0, 0, 0, 255)
    clut[1:1 + len(pal), :3] = pal
    clut[1:1 + len(pal), 3] = 255
    clut[1 + len(pal):] = (0, 0, 0, 255)
    # triangle subsets: near a light horizontally, inside its vertical window, opaque batches only
    subs, sub_idx, cand = [], [], []
    n_lit, n_all = 0, 0
    for bi, b in enumerate(sc["batches"]):
        tri = sc["idx"][b["ist"]:b["ist"] + b["ic"]].reshape(-1, 3).astype(np.int64)
        n_all += len(tri)
        if b["flags"] & 1:
            continue
        tp = P[b["vs"] + tri]                                     # (n,3,3)
        keep = np.zeros(len(tri), bool)
        for L in lights:
            dxz = np.hypot(tp[..., 0] - L.pos[0], tp[..., 2] - L.pos[2]).min(1)
            ymin, ymax = tp[..., 1].min(1), tp[..., 1].max(1)
            keep |= (dxz < L.radius) & (ymax > L.pos[1] + V_WINDOW[0]) & (ymin < L.pos[1] + V_WINDOW[1])
        # drop triangles the lightmap barely touches (< MIN_GAIN brighter at every corner and the centre)
        smp = np.concatenate([tp, tp.mean(1, keepdims=True)], 1)          # (n,4,3)
        ui = np.clip(((smp[..., 0] - x0) / texel).astype(np.int64), 0, w - 1)
        vi = np.clip(((smp[..., 2] - z0) / texel).astype(np.int64), 0, h - 1)
        score = acc[vi, ui].max(-1).max(-1)
        keep &= score > MIN_GAIN
        cand.append((bi, tri, keep, score))
    # budget: the pass costs vertices + fill (hidden triangles too: they are rasterised, then Z-rejected), so keep
    # only the max_pass_tris brightest triangles of the whole scene [PSP-est: pit 10k tris -> +12..26 ms]
    allsc = np.concatenate([sc_[k] for _, _, k, sc_ in cand]) if cand else np.zeros(0)
    thr = np.sort(allsc)[-max_pass_tris] if len(allsc) > max_pass_tris else -1.0
    for bi, tri, keep, score in cand:
        keep = keep & (score >= thr)
        if keep.any():
            subs.append((bi, sum(len(s) for s in sub_idx), int(keep.sum()) * 3))
            sub_idx.append(tri[keep].reshape(-1).astype(np.uint16))
            n_lit += int(keep.sum())
    stats = dict(lights=len(lights), lm=[w, h], texel_m=round(texel, 3), rect=[round(float(x0), 2), round(float(z0), 2),
                 round(float(sx), 2), round(float(sz), 2)], lit_tris=n_lit, scene_tris=n_all,
                 lit_frac=round(n_lit / max(n_all, 1), 3), subsets=len(subs))
    return dict(sc=sc, lights=lights, lm=(w, h, x0, z0, sx, sz), clut=clut, idx=idx.astype(np.uint8), subs=subs,
                sub_idx=np.concatenate(sub_idx) if sub_idx else np.zeros(0, np.uint16), stats=stats,
                acc=acc, col_tris=col_tris)


def write_ashl(path: Path, bk: dict, fog: dict, chl: dict):
    w, h, x0, z0, sx, sz = bk["lm"]
    lights, subs, sidx = bk["lights"], bk["subs"], bk["sub_idx"]
    buf = bytearray(HDR_SIZE)

    def align():
        while len(buf) % 16:
            buf.append(0)
    align(); off_light = len(buf)
    rng = np.random.default_rng(18)
    for L in lights:
        buf += struct.pack("<4fI2H2f", *[float(v) for v in L.pos], L.radius, _abgr(L.color * 255), L.kind, 0,
                           L.strength, float(rng.uniform(0, 6.283)))
    align(); off_sub = len(buf)
    for bi, st, n in subs:
        buf += struct.pack("<4I", bi, st, n, 0)
    align(); off_idx = len(buf)
    buf += sidx.astype("<u2").tobytes()
    align(); off_clut = len(buf)
    clut = bk["clut"]
    buf += np.stack([clut[:, 0], clut[:, 1], clut[:, 2], clut[:, 3]], 1).astype(np.uint8).tobytes()  # R,G,B,A bytes = ABGR u32 LE
    align(); off_pix = len(buf)
    buf += swizzle(bk["idx"], 5)
    align()
    d = chl["dirs"]
    hdr = struct.pack("<4s7I8I4f2fII9f3II f", MAGIC, VERSION, len(lights), len(subs), len(sidx), w, h, 0,
                      off_light, off_sub, off_idx, off_clut, off_pix, 0, 0, 0,
                      float(x0), float(z0), float(sx), float(sz),
                      float(fog["near"]), float(fog["far"]), fog["color"], fog["clear"],
                      *[float(v) for v in d[0]], *[float(v) for v in d[1]], *[float(v) for v in d[2]],
                      *chl["colors"], chl["ambient"], 1.0)
    assert len(hdr) <= HDR_SIZE, len(hdr)
    buf[:len(hdr)] = hdr
    path.write_bytes(bytes(buf))
    return len(buf)


def vertex_bake(bk: dict, out: Path):
    """Copy of the .ashm with the lights multiplied into the vertex colours (no runtime cost)."""
    sc = bk["sc"]
    data = bytearray(sc["data"])
    vert = np.frombuffer(data, sc["vert"].dtype, count=len(sc["vert"]), offset=sc["off_vert"]).copy()
    P = vert["pos"].astype(np.float32)
    # vertex normals from the triangles
    N = np.zeros_like(P)
    for b in sc["batches"]:
        tri = sc["idx"][b["ist"]:b["ist"] + b["ic"]].reshape(-1, 3).astype(np.int64) + b["vs"]
        fn = np.cross(P[tri[:, 1]] - P[tri[:, 0]], P[tri[:, 2]] - P[tri[:, 0]])
        for k in range(3):
            np.add.at(N, tri[:, k], fn)
    N /= np.maximum(np.linalg.norm(N, axis=1, keepdims=True), 1e-6)
    lit = light_at(P + N * 0.05, N, bk["lights"], bk["col_tris"])
    c = vert["color"]
    rgb = np.stack([c & 255, (c >> 8) & 255, (c >> 16) & 255], 1).astype(np.float32)
    rgb = np.clip(rgb + lit * 255.0 * VERT_GAIN, 0, 255).astype(np.uint32)   # additive: x texture = albedo light
    vert["color"] = (c & 0xff000000) | (rgb[:, 2] << 16) | (rgb[:, 1] << 8) | rgb[:, 0]
    data[sc["off_vert"]:sc["off_vert"] + vert.nbytes] = vert.tobytes()
    out.write_bytes(bytes(data))
    return int((lit.sum(1) > 1e-3).sum())


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--scene", type=Path, required=True, help=".ashm")
    ap.add_argument("--pcol", type=Path, required=True, help="Havok collision of the same place (shadows, floors)")
    ap.add_argument("--map", default="m18_01_00_00")
    ap.add_argument("--area", default="a18", help="DrawParam bank a<NN>")
    ap.add_argument("--floor", default="h0005B1", help="collision whose MSB fog/light ids give the zone look")
    ap.add_argument("--max-pass-tris", type=int, default=2500, help="second-pass triangle budget (brightest kept)")
    ap.add_argument("--lm", type=int, default=128, help="max lightmap side (texels)")
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--vertex-out", type=Path, help="also write a vertex-baked copy of the .ashm")
    ap.add_argument("--preview", action="store_true", help="<out>.png of the lightmap (outside git)")
    a = ap.parse_args(argv)
    lights = msb_lights(a.map, a.area)
    bk = bake(a.scene, a.pcol, lights, a.lm, a.max_pass_tris)
    fog = zone_fog(a.map, a.area, a.floor)
    chl = zone_char_light(a.map, a.area, a.floor)
    size = write_ashl(a.out, bk, fog, chl)
    st = bk["stats"]
    st.update(bytes=size, fog=fog, char_light=chl,
              light_list=[dict(name=L.name, pos=[round(float(v), 2) for v in L.pos], r=round(L.radius, 2),
                               row=L.row, strength=L.strength) for L in bk["lights"]])
    if a.vertex_out:
        st["vertex_lit_verts"] = vertex_bake(bk, a.vertex_out)
    if a.preview:
        from PIL import Image
        Image.fromarray(np.clip(bk["acc"] * ADD_GAIN * 255, 0, 255).astype(np.uint8)).resize(
            (bk["lm"][0] * 2, bk["lm"][1] * 2)).save(a.out.with_suffix(".light.png"))
    a.out.with_suffix(".light.json").write_text(json.dumps(st, ensure_ascii=False, indent=1, default=str))
    print(f"cook_light: {a.out.name}: {st['lights']} lights, lightmap {st['lm'][0]}x{st['lm'][1]} "
          f"({st['texel_m']} m/texel), second pass {st['lit_tris']}/{st['scene_tris']} tris "
          f"({100 * st['lit_frac']:.1f}%) in {st['subsets']} draws, {size / 1024:.1f} KB")
    print(f"  fog: {fog['src']}: near {fog['near']:.0f} far {fog['far']:.0f} color {fog['color']:08x}")
    print(f"  char light: {chl['src']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
