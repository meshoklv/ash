#!/usr/bin/env python3
"""Cook v0: one Undead Asylum scene (map pieces placed by MSB + Elite Knight + Asylum Demon) -> one `.ashm`.

    tools/.venv/bin/python tools/cook_scene.py --scene hall --out cooked/hall.ashm

Map lighting: DSR per-piece lightmap (g_Lightmap, sampled at UV1) baked into vertex colours.
Characters: bind pose (or a pose from tools/pose.py if available), simple baked lambert.
Everything is cooked in FromSoft space and mirrored (x -> -x) at the very end: FromSoft front faces are CW in
a left-handed world, after the mirror they are CCW in a right-handed one (indices are not reordered).
"""
from __future__ import annotations

import argparse
import json
import math
import sys
import time
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).parent))
from ashm import BF_ALPHA, BF_NOCULL, Batch, Scene, cook_texture, merge_batches, simplify, tessellate, weld, write_ashm  # noqa: E402
from dsrlib import (DSR, FLVER, Binder, binder_flver, binder_textures, chr_textures, load_map_texture, mesh_arrays,  # noqa: E402
                    mesh_textures, mesh_triangles, valid_meshes)

SKIP_MTD = ("Water", "_Edge", "Fly", "Snow", "Light", "Shadow", "Mirror")

SCENES = {
    # Asylum Demon boss arena: the upper hall (MSB c2232_0000 at (3.0, 198.06, -29.54), rot 180). The "hall" scene
    # below is the pit under it (Stray Demon c2230). Pieces = those whose bounds touch the arena box (probe).
    "arena": dict(
        map="m18_01_00_00", area="m18",
        # m0200 = floor, walls, pillars and the gates of the hall; m0210 = floor details. Upper walls/roof
        # (m0201, m0041, m0045, m0046, m0140, m0150, m5100, m0130) left out: pyfqmr turns m0200 into shards
        # even at 50 % of its 28.8k tris, so the arena is cooked unsimplified and has to stay small.
        pieces=["m0200B1", "m0210B1"],
        map_budget=10**9, max_edge=3.0,
        objects=["o8510_0000"],  # the middle of the floor (the Asylum Demon breaks it later), intact state
        demon=dict(pos=(3.0, 198.06, -29.54), yaw=180.0),
        knight=dict(pos=(3.0, 198.0, -12.0), yaw=0.0),  # [придумал] north part of the hall, facing the demon
    ),
    "hall": dict(
        map="m18_01_00_00", area="m18",
        pieces=["m0300B1", "m0301B1", "m0310B1", "m0320B1"],
        map_budget=16000, max_edge=3.0,
        # characters: (key, world pos FS space, yaw deg)
        demon=dict(pos=(3.31, 182.11, -9.46), yaw=0.0),  # MSB: rot 0; DS models face -Z,
        knight=dict(pos=(3.0, 182.05, -24.0), yaw=180.0),  # on stone floor (ray-cast), facing the demon,
    ),
}

# --------------------------------------------------------------------------- helpers


def bilinear(img: np.ndarray, uv: np.ndarray) -> np.ndarray:
    h, w = img.shape[:2]
    x = (uv[:, 0] % 1.0) * w - 0.5
    y = (uv[:, 1] % 1.0) * h - 0.5
    x0 = np.floor(x).astype(int); y0 = np.floor(y).astype(int)
    fx = (x - x0)[:, None]; fy = (y - y0)[:, None]
    x0 %= w; y0 %= h; x1 = (x0 + 1) % w; y1 = (y0 + 1) % h
    a = img[y0, x0] * (1 - fx) + img[y0, x1] * fx
    b = img[y1, x0] * (1 - fx) + img[y1, x1] * fx
    return a * (1 - fy) + b * fy


def yaw_matrix(deg: float) -> np.ndarray:
    a = math.radians(deg)
    c, s = math.cos(a), math.sin(a)
    return np.array([[c, 0, s], [0, 1, 0], [-s, 0, c]], np.float32)


class TexBank:
    def __init__(self, max_size=128):
        self.items = []
        self.index = {}
        self.max_size = max_size

    def add(self, key, loader, max_size=None):
        if key in self.index:
            return self.index[key]
        im = loader()
        if im is None:
            return None
        t = cook_texture(im, max_size or self.max_size)
        t.src = (key, im.size)
        self.index[key] = len(self.items)
        self.items.append(t)
        return self.index[key]


# --------------------------------------------------------------------------- map


def cook_map_pieces(cfg, bank: TexBank, stats: dict) -> list[Batch]:
    area = cfg["area"]
    lm_cache = {}
    by_tex: dict[int, list[Batch]] = {}
    src_tris = 0
    for piece in cfg["pieces"]:
        f = FLVER.from_path(DSR / "map" / cfg["map"] / f"{piece}A{area[1:]}.flver.dcx")
        offset = np.array([0.0, 200.0, 0.0], np.float32)  # all m18_01 pieces sit at (0,200,0), rot 0 (probe.py)
        for m in valid_meshes(f):
            mtd = m.material.mat_def_path.rsplit("\\", 1)[-1]
            texs = mesh_textures(m)
            if any(s in mtd for s in SKIP_MTD) or not texs.get("g_Diffuse"):
                continue
            d = mesh_arrays(m)
            tri = mesh_triangles(m)
            if len(tri) == 0:
                continue
            src_tris += len(tri)
            dstem = texs["g_Diffuse"]
            ti = bank.add(("map", dstem), lambda: (load_map_texture(area, dstem) or (None,))[0])
            if ti is None:
                continue
            # light is sampled AFTER simplify+tessellate, per final vertex: carry UV1 in the colour slot meanwhile
            lstem = texs.get("g_Lightmap") if "uv1" in d else None
            if lstem and lstem not in lm_cache:
                r = load_map_texture(area, lstem)
                lm_cache[lstem] = None if r is None else np.asarray(r[0].convert("RGB"), np.float32) / 255.0
            if lstem and lm_cache[lstem] is None:
                lstem = None
            carry = np.zeros((len(d["pos"]), 3), np.float32)
            if lstem:
                carry[:, :2] = d["uv1"]
            flags = BF_ALPHA if bank.items[ti].alpha else 0
            if "_Alp" in mtd or flags:
                flags |= BF_NOCULL
            b = Batch(ti, d["pos"] + offset, d["uv0"], carry, tri, flags, f"{piece}:{dstem}")
            by_tex.setdefault((ti, flags, lstem), []).append(weld(b))
    keys = list(by_tex)
    merged = [merge_batches(by_tex[k]) for k in keys]
    total = sum(b.ntri for b in merged)
    ratio = min(1.0, cfg["map_budget"] / max(1, total))
    out = []
    for key, b in zip(keys, merged):
        s = simplify(b, max(12, int(b.ntri * ratio)))
        t = tessellate(s, cfg["max_edge"])
        lstem = key[2]
        if lstem:
            col = bilinear(lm_cache[lstem], t.col[:, :2])
            col = col * cfg.get("lm_scale", 3.8) + cfg.get("lm_ambient", 0.16)
            t.col = (col / np.maximum(1.0, col.max(1, keepdims=True))).astype(np.float32)  # keep hue, no yellow clip
        else:
            t.col = np.full_like(t.col, 0.35)
        out.append(t)
    stats["map"] = dict(pieces=cfg["pieces"], src_tris=src_tris, merged_batches=len(merged), welded_tris=total,
                        simplified_tris=sum(b.ntri for b in out))
    return out


def cook_objects(cfg, bank: TexBank, stats: dict) -> list[Batch]:
    """MSB objects drawn as part of the map (first FLVER of the objbnd, placed by MSB translate + yaw).
    No lightmap: flat colour close to the lit floor [придумал]."""
    from soulstruct.darksouls1r.maps import MSB
    names = cfg.get("objects", [])
    if not names:
        return []
    msb = MSB.from_path(DSR / "map" / "MapStudio" / f"{cfg['map']}.msb")
    out = []
    for o in msb.objects:
        if o.name not in names:
            continue
        b = Binder.from_path(DSR / "obj" / f"{o.model.name}.objbnd.dcx")
        f = binder_flver(b)
        tx = binder_textures(b)
        R = yaw_matrix(float(o.rotate[1]))
        t = np.array([float(v) for v in o.translate], np.float32)
        for m in valid_meshes(f):
            dstem = mesh_textures(m).get("g_Diffuse")
            loader = (lambda d=dstem: tx[d][0]) if dstem in tx else (lambda d=dstem: (load_map_texture(cfg["area"], d) or (None,))[0])
            ti = bank.add(("obj", dstem), loader)
            if ti is None:
                continue
            d = mesh_arrays(m)
            tri = mesh_triangles(m)
            flags = BF_ALPHA | BF_NOCULL if bank.items[ti].alpha else 0
            col = np.full((len(d["pos"]), 3), cfg.get("obj_light", 0.42), np.float32)
            bt = tessellate(weld(Batch(ti, d["pos"] @ R.T + t, d["uv0"], col, tri, flags, f"{o.name}:{dstem}")),
                            cfg["max_edge"])
            out.append(bt)
    stats["objects"] = dict(names=names, tris=sum(b.ntri for b in out))
    return out


# --------------------------------------------------------------------------- characters


def skeleton_c0000():
    f = binder_flver(Binder.from_path(DSR / "chr/c0000.chrbnd.dcx"))
    names = [b.name for b in f.bones]
    tr = f.get_bone_tree().get_bone_armature_space_transforms()
    mats = {}
    for n, (t, r, s) in zip(names, tr):
        mats[n] = (np.array(list(t), np.float32), np.array(r.data, np.float32))
    return mats


LIGHT_DIR = np.array([0.35, 0.8, -0.45], np.float32)
LIGHT_DIR /= np.linalg.norm(LIGHT_DIR)


def lambert(normals: np.ndarray, tint=(1.0, 0.82, 0.62), amb=0.28) -> np.ndarray:
    n = normals / (np.linalg.norm(normals, axis=1, keepdims=True) + 1e-9)
    d = np.clip(n @ LIGHT_DIR, 0, 1)[:, None]
    return np.clip(amb + d * np.array(tint, np.float32) * 0.85, 0, 1).astype(np.float32)


def cook_flver_char(f: FLVER, textures: dict, bank: TexBank, key: str, xform, budget: int, tex_size=128,
                     rigid_attach=None, pose=None) -> tuple[list[Batch], int]:
    """xform(pos (N,3), normal (N,3)) -> world-space (FS) pos, normal.
    rigid_attach: (t, R) armature transform for non-dynamic meshes (weapons).
    pose: optional callable(mesh_arrays, mesh) -> (pos, normal) in armature space (skinned)."""
    parts = []
    src = 0
    for m in valid_meshes(f):
        texs = mesh_textures(m)
        dstem = texs.get("g_Diffuse")
        if not dstem or dstem not in textures:
            continue
        d = mesh_arrays(m)
        tri = mesh_triangles(m)
        src += len(tri)
        pos, nrm = d["pos"], d.get("normal", np.tile([0, 1, 0], (len(d["pos"]), 1)).astype(np.float32))
        if not m.is_dynamic:
            bi = m.default_bone_index if m.default_bone_index >= 0 else 0
            bt = f.get_bone_tree().get_bone_armature_space_transforms()[bi]
            Rb = np.array(bt[1].data, np.float32); tb = np.array(list(bt[0]), np.float32)
            pos = pos @ Rb.T + tb; nrm = nrm @ Rb.T
            if rigid_attach is not None:
                ta, Ra = rigid_attach
                pos = pos @ Ra.T + ta; nrm = nrm @ Ra.T
        elif pose is not None:
            pos, nrm = pose(d, m)
        pos, nrm = xform(pos, nrm)
        ti = bank.add((key, dstem), lambda: textures[dstem][0], tex_size)
        flags = BF_ALPHA if bank.items[ti].alpha else 0
        if flags:
            flags |= BF_NOCULL
        parts.append(Batch(ti, pos.astype(np.float32), d["uv0"], lambert(nrm), tri, flags, f"{key}:{dstem}"))
    total = sum(p.ntri for p in parts)
    ratio = min(1.0, budget / max(1, total))
    out = [simplify(weld(p), max(8, int(p.ntri * ratio))) for p in parts]
    return out, src


def place(pos, yaw):
    R = yaw_matrix(yaw)
    t = np.array(pos, np.float32)
    return lambda p, n: (p @ R.T + t, n @ R.T)


def cook_knight(cfg, bank, stats, pose=None):
    sk = skeleton_c0000()
    xf = place(cfg["knight"]["pos"], cfg["knight"]["yaw"])
    out, src = [], 0
    budget = cfg.get("knight_budget", 3000)
    # budgets per part ~ proportional to source (15.3k tris total)
    for part, share in (("HD_A_9500", 0.12), ("BD_A_9500", 0.38), ("AM_A_9500", 0.2), ("LG_M_9500", 0.22),
                        ("WP_A_0201", 0.03), ("WP_A_1516", 0.05)):
        b = Binder.from_path(DSR / "parts" / f"{part}.partsbnd.dcx")
        f = binder_flver(b)
        tx = binder_textures(b)
        if pose is not None:
            pose.part_bone_names = [bn.name for bn in f.bones]
        attach = None
        if part.startswith("WP_A_02"):
            attach = sk["R_Weapon"] if pose is None else pose.bone("R_Weapon")
        elif part.startswith("WP_A_15"):
            attach = sk["L_Weapon"] if pose is None else pose.bone("L_Weapon")
        bs, s = cook_flver_char(f, tx, bank, part, xf, int(budget * share), 128, attach,
                                None if pose is None else pose.skin)
        out += bs; src += s
    stats["knight"] = dict(src_tris=src, tris=sum(b.ntri for b in out),
                           pose=None if pose is None else dict(src=pose.source, frames=pose.frame_count,
                                                               flver_bones_without_track=len(pose.missing)))
    return out


def cook_demon(cfg, bank, stats, anim=""):
    b = Binder.from_path(DSR / "chr/c2230.chrbnd.dcx")
    f = binder_flver(b)
    pose = None
    if anim:
        from pose import CharPose
        pose = CharPose("c2230", anim)
    tx = chr_textures("c2230")
    tx.update(chr_textures("c2239"))  # the demon's body texture lives in the shared c2239 texture pack
    xf = place(cfg["demon"]["pos"], cfg["demon"]["yaw"])
    out, src = cook_flver_char(f, tx, bank, "c2230", xf, cfg.get("demon_budget", 5000), 256, None,
                               None if pose is None else pose.skin)
    stats["demon"] = dict(src_tris=src, tris=sum(b.ntri for b in out),
                          pose=None if pose is None else dict(src=pose.source, frames=pose.frame_count))
    return out


# --------------------------------------------------------------------------- main


def write_heightgrid(path: Path, batches: list[Batch], cfg: dict, cell: float = 0.5) -> dict:
    """Floor height grid (PSP space) from the *render* map mesh (stand-in until the HKX collision cooker):
    per cell the highest upward-facing surface below `floor_ceiling` (keeps balconies/arches out).
    .ashh v1: "ASHH", u32 version=1, u32 nx, u32 nz, f32 x0, z0, cell, pad; f32 h[nz][nx] (-1e9 = no floor)."""
    import struct
    ceil_y = cfg.get("floor_ceiling", 186.5)
    tris = []
    for b in batches:
        P = b.pos[b.tri]
        n = np.cross(P[:, 1] - P[:, 0], P[:, 2] - P[:, 0])
        n /= np.linalg.norm(n, axis=1, keepdims=True) + 1e-12
        up = np.abs(n[:, 1]) > 0.55
        tris.append(P[up])
    P = np.concatenate(tris)
    P = P[(P[:, :, 1].min(1) < ceil_y)]
    lo = P[:, :, [0, 2]].reshape(-1, 2).min(0); hi = P[:, :, [0, 2]].reshape(-1, 2).max(0)
    x0, z0 = float(lo[0]), float(lo[1])
    nx, nz = int(np.ceil((hi[0] - x0) / cell)) + 1, int(np.ceil((hi[1] - z0) / cell)) + 1
    H = np.full((nz, nx), -1e9, np.float32)
    for t in P:
        xs, zs = t[:, 0], t[:, 2]
        i0, i1 = int(np.floor((xs.min() - x0) / cell)), int(np.ceil((xs.max() - x0) / cell))
        j0, j1 = int(np.floor((zs.min() - z0) / cell)), int(np.ceil((zs.max() - z0) / cell))
        gx, gz = np.meshgrid(x0 + np.arange(i0, i1 + 1) * cell, z0 + np.arange(j0, j1 + 1) * cell)
        a, b2, c = t[0], t[1], t[2]
        d = (b2[2] - c[2]) * (a[0] - c[0]) + (c[0] - b2[0]) * (a[2] - c[2])
        if abs(d) < 1e-9:
            continue
        w0 = ((b2[2] - c[2]) * (gx - c[0]) + (c[0] - b2[0]) * (gz - c[2])) / d
        w1 = ((c[2] - a[2]) * (gx - c[0]) + (a[0] - c[0]) * (gz - c[2])) / d
        w2 = 1 - w0 - w1
        inside = (w0 >= -1e-4) & (w1 >= -1e-4) & (w2 >= -1e-4)
        h = w0 * a[1] + w1 * b2[1] + w2 * c[1]
        ok = inside & (h < ceil_y)
        sub = H[j0:j1 + 1, i0:i1 + 1]
        sub[ok] = np.maximum(sub[ok], h[ok])
    body = struct.pack("<4s3I3fI", b"ASHH", 1, nx, nz, x0, z0, cell, 0) + H.astype("<f4").tobytes()
    path.write_bytes(body)
    return dict(nx=nx, nz=nz, x0=round(x0, 2), z0=round(z0, 2), cell=cell, bytes=len(body),
                filled=round(float((H > -1e8).mean()), 3))


def to_psp_space(sc: Scene):
    for b in sc.batches:
        b.pos = b.pos * np.array([-1, 1, 1], np.float32)
    sc.cameras = [((-e[0], e[1], e[2]), (-t[0], t[1], t[2]), fov) for e, t, fov in sc.cameras]


def cameras_for(cfg):
    k = np.array(cfg["knight"]["pos"], np.float32)
    dmn = np.array(cfg["demon"]["pos"], np.float32)
    fwd = dmn - k; fwd[1] = 0; fwd /= np.linalg.norm(fwd)
    right = np.cross(fwd, [0, 1, 0]).astype(np.float32)
    cams = []
    # 0: over the shoulder, looking at the demon
    eye = k - fwd * 3.6 + right * 0.9 + np.array([0, 2.3, 0], np.float32)
    cams.append((tuple(eye), tuple(dmn + [0, 3.2, 0]), 62.0))
    # 1: side view of the standoff
    mid = (k + dmn) / 2
    eye = mid + right * 13.0 + np.array([0, 4.0, 0], np.float32)
    cams.append((tuple(eye), tuple(mid + [0, 2.5, 0]), 60.0))
    # 2: knight close-up from the front (demon behind the camera)
    eye = k + fwd * 2.6 - right * 0.8 + np.array([0, 1.6, 0], np.float32)
    cams.append((tuple(eye), tuple(k + [0, 1.15, 0]), 50.0))
    # 3: three-quarter view from the other side, slightly above
    eye = mid - right * 10.0 - fwd * 4.0 + np.array([0, 5.5, 0], np.float32)
    cams.append((tuple(eye), tuple((k + dmn) / 2 + [0, 0.5, 0]), 64.0))
    return cams


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--scene", default="hall")
    ap.add_argument("--out", required=True)
    ap.add_argument("--no-chars", action="store_true")
    ap.add_argument("--heightgrid", default="", help="also write a floor height grid (.ashh) from the map mesh")
    ap.add_argument("--no-knight", action="store_true", help="leave the knight out (it is drawn skinned at runtime, tools/cook_knight.py)")
    ap.add_argument("--debug-marker", action="store_true", help="red box at knight + 3 m along FromSoft +X")
    ap.add_argument("--demon-pose", default="a00_0000", help="c2230 animation id, empty = bind pose")
    ap.add_argument("--map-budget", type=int, default=None, help="override map triangle budget (0 = no simplification)")
    ap.add_argument("--pose", default="a00_0000", help="c0000 animation id for the knight (e.g. a00_0000), empty = bind pose")
    ap.add_argument("--pieces", default="", help="comma-separated map pieces instead of the scene's list")
    args = ap.parse_args()
    cfg = dict(SCENES[args.scene])
    if args.pieces:
        cfg["pieces"] = args.pieces.split(",")
    if args.map_budget is not None:
        cfg["map_budget"] = args.map_budget or 10**9
    t0 = time.time()
    bank = TexBank(128)
    stats: dict = {"scene": args.scene}
    sc = Scene()
    sc.batches += cook_map_pieces(cfg, bank, stats)
    sc.batches += cook_objects(cfg, bank, stats)
    n_map = len(sc.batches)
    if not args.no_chars:
        pose = None
        if args.pose and not args.no_knight:
            from pose import KnightPose
            pose = KnightPose(args.pose)
        if not args.no_knight:
            sc.batches += cook_knight(cfg, bank, stats, pose)
        sc.batches += cook_demon(cfg, bank, stats, args.demon_pose)
    if args.debug_marker:
        # red box 3 m to FS +X of the knight: checks handedness of the whole chain on screen
        from PIL import Image
        ti = bank.add(("debug", "red"), lambda: Image.new("RGBA", (16, 16), (255, 0, 0, 255)))
        c = np.array(cfg["knight"]["pos"], np.float32) + [3, 1, 0]
        v = np.array([[x, y, z] for x in (-.4, .4) for y in (-.4, .4) for z in (-.4, .4)], np.float32) + c
        q = [(0, 1, 3, 2), (4, 6, 7, 5), (0, 4, 5, 1), (2, 3, 7, 6), (0, 2, 6, 4), (1, 5, 7, 3)]
        tri = np.array([t for a, b2, c2, d2 in q for t in ((a, b2, c2), (a, c2, d2))])
        sc.batches.append(Batch(ti, v, np.zeros((8, 2), np.float32), np.ones((8, 3), np.float32), tri, BF_NOCULL))
    sc.textures = bank.items
    sc.cameras = cameras_for(cfg)
    to_psp_space(sc)
    if args.heightgrid:
        stats["heightgrid"] = write_heightgrid(Path(args.heightgrid), sc.batches[:n_map], cfg)
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    stats["file"] = write_ashm(out, sc)
    stats["textures"] = [dict(key=str(t.src[0]), src=t.src[1], cooked=(t.w, t.h), alpha=t.alpha) for t in bank.items]
    stats["cook_seconds"] = round(time.time() - t0, 1)
    out.with_suffix(".json").write_text(json.dumps(stats, indent=1, default=str))
    s = {k: v for k, v in stats.items() if k != "textures"}
    print(json.dumps(s, indent=1, default=str))


if __name__ == "__main__":
    main()
