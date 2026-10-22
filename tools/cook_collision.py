#!/usr/bin/env python3
"""Cook the Havok map collision of one area of a DSR map into `.pcol` (player/camera collision for the PSP).

    tools/.venv/bin/python tools/cook_collision.py --scene hall --out ~/ash-assets/cooked/hall.pcol

Source: `map/<map>/h<map>.hkxbhd/.hkxbdt` (hi-res collision, the one the player walks on) read with
soulstruct-havok `MapCollisionModel` (pure Python). Placement of every collision part = its MSB `collisions` entry
(translate + rotate; all of m18_01 sit at (0, 200, 0) with no rotation). Triangles whose bounding box touches the
scene box are kept, welded per part, mirrored to PSP space (x -> -x; FromSoft CW front faces become CCW, the same
convention as tools/cook_scene.py, so cross(b - a, c - a) is the outward normal).

`.pcol` v1, little-endian, sections 16-aligned, offsets from file start:
  Header 80 B: "PCOL", u32 version=1, n_vert, n_tri, n_part, nx, nz, n_list,
               f32 x0, z0, cell, pad,
               u32 off_vert, off_tri, off_part, off_cell, off_list, pad[3]
  Vertex 12 B: f32 x, y, z (PSP space)
  Tri 10 B:    u16 v[3], u16 material (MapCollisionMaterial, +100/+200/+300 = stairs variants), u8 part, u8 flags
  Part 24 B:   char name[16], s32 cam_row (MSB camera_1_id, -1 = default LockCamParam row), u32 pad
  Cell 8 B:    u32 start, u32 count -> into the u16 triangle list; nx * nz cells of `cell` m over XZ, row-major (z, x)
  List:        u16 triangle indices
Next to it `<out>.spawn` (text, PSP space, yaw in radians, PSP yaw = -FromSoft yaw):
  player x y z yaw          (scene config; y is snapped to the floor at runtime)
  target <model> x y z yaw  (MSB character entry `target`)
The runtime computes normals/planes at load. Never committed (FromSoftware data).
"""
from __future__ import annotations

import argparse
import json
import logging
import struct
import sys
import time
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).parent))
logging.disable(logging.WARNING)
from dsrlib import DSR, Binder  # noqa: E402

SCENES = {
    # Asylum Demon boss arena (upper hall, floor h0005B1 y ~197.6-198, LockCamParam 103). Target = MSB c2232_0000
    # (Asylum Demon, NpcParam 223200). Player spot: north part of the hall facing the demon [придумал].
    "arena": dict(map="m18_01_00_00", box_min=(-9.5, 196.0, -44.0), box_max=(16.5, 226.0, -0.5), cell=2.0,
                  skip=(), target="c2232_0000", player=(3.0, 199.0, -12.0, 0.0),
                  # the middle of the floor is an object (o8510, the floor the Asylum Demon later breaks): its
                  # collision lives in obj/o8510.objbnd (o8510.hkx, intact state), not in the map HKX
                  objects=("o8510_0000",)),
    # The pit under it (Stray Demon c2230, y 182.25) - the first M4a test scene: floor h0016B1 (y 182.25), side aisles h1600B1/h1601B1, ceiling ~197.6 (FromSoft space).
    "hall": dict(map="m18_01_00_00", box_min=(-13.5, 180.0, -41.0), box_max=(19.5, 199.0, 4.5), cell=2.0,
                 skip=(), target="c2230_0000", player=(3.0, 183.0, -24.0, 180.0)),
}


def euler_matrix(deg) -> np.ndarray:
    """MSB rotate (x, y, z degrees). FromSoft order: R = Ry * Rz * Rx [версия: all m18_01 collisions have 0]."""
    x, y, z = np.radians(deg)
    cx, sx, cy, sy, cz, sz = np.cos(x), np.sin(x), np.cos(y), np.sin(y), np.cos(z), np.sin(z)
    Rx = np.array([[1, 0, 0], [0, cx, -sx], [0, sx, cx]])
    Ry = np.array([[cy, 0, sy], [0, 1, 0], [-sy, 0, cy]])
    Rz = np.array([[cz, -sz, 0], [sz, cz, 0], [0, 0, 1]])
    return Ry @ Rz @ Rx


def load_parts(map_id: str):
    from soulstruct.darksouls1r.maps import MSB
    from soulstruct.havok.fromsoft.darksouls1r import MapCollisionModel
    area = map_id[1:3]
    d = DSR / "map" / map_id
    b = Binder.from_path(d / f"h{map_id[1:]}.hkxbhd", d / f"h{map_id[1:]}.hkxbdt")
    models = {}
    for e in b.entries:
        stem = e.name.replace("\\", "/").rsplit("/", 1)[-1].split(".")[0]   # h0016B1A18
        models[stem[: -len(f"A{area}")] if stem.endswith(f"A{area}") else stem] = e
    msb = MSB.from_path(DSR / "map" / "MapStudio" / f"{map_id}.msb")
    out = []
    for c in msb.collisions:
        model = c.model.name if c.model else None
        if model not in models:
            continue
        hk = MapCollisionModel.from_bytes(models[model].data)
        R = euler_matrix([float(v) for v in c.rotate])
        t = np.array([float(v) for v in c.translate])
        meshes = [(mm.vertices3D.astype(np.float64) @ R.T + t, mm.face_vertex_indices.astype(np.int64),
                   int(mm.material_index)) for mm in hk.meshes]
        out.append(dict(name=c.name, model=model, cam_row=int(getattr(c, "camera_1_id", -1)), meshes=meshes))
    return out


def load_objects(map_id: str, names) -> list:
    """MSB objects whose objbnd holds a map-style collision `<model>.hkx` (e.g. breakable floors)."""
    from soulstruct.darksouls1r.maps import MSB
    from soulstruct.havok.fromsoft.darksouls1r import MapCollisionModel
    msb = MSB.from_path(DSR / "map" / "MapStudio" / f"{map_id}.msb")
    out = []
    for o in msb.objects:
        if o.name not in names:
            continue
        model = o.model.name
        b = Binder.from_path(DSR / "obj" / f"{model}.objbnd.dcx")
        e = next(e for e in b.entries if e.name.replace("\\", "/").rsplit("/", 1)[-1].lower() == f"{model}.hkx")
        hk = MapCollisionModel.from_bytes(e.data)
        R = euler_matrix([float(v) for v in o.rotate])
        t = np.array([float(v) for v in o.translate])
        meshes = [(mm.vertices3D.astype(np.float64) @ R.T + t, mm.face_vertex_indices.astype(np.int64),
                   int(mm.material_index)) for mm in hk.meshes]
        out.append(dict(name=o.name, model=model, cam_row=-1, meshes=meshes))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--scene", default="hall")
    ap.add_argument("--out", required=True)
    args = ap.parse_args()
    t0 = time.time()
    cfg = SCENES[args.scene]
    lo, hi = np.array(cfg["box_min"]), np.array(cfg["box_max"])
    parts = load_parts(cfg["map"])
    objs = load_objects(cfg["map"], cfg.get("objects", ()))
    for o in objs:  # LockCamParam row of an object = row of the map collision floor around it (DS1 picks by floor)
        c = np.concatenate([m[0] for m in o["meshes"]]).mean(0)
        best = None
        for p in parts:
            V = np.concatenate([m[0] for m in p["meshes"]])
            lo_, hi_ = V.min(0), V.max(0)
            if lo_[0] <= c[0] <= hi_[0] and lo_[2] <= c[2] <= hi_[2] and abs(lo_[1] - c[1]) < 1.5:
                best = p if best is None or abs(lo_[1] - c[1]) < abs(best["_y"] - c[1]) else best
                if best is p:
                    p["_y"] = lo_[1]
        if best is not None:
            o["cam_row"] = best["cam_row"]
            o["cam_from"] = best["name"]
    parts += objs
    seen_models = set()
    V, T, P = [], [], []
    stats = dict(scene=args.scene, parts=[], src_tris_in_map=0)
    nv = 0
    for p in parts:
        if p["name"] in cfg["skip"]:
            continue
        # two MSB parts may share one model at the same place (h0399B1 / h0399B1_0000): keep one
        key = (p["model"], tuple(np.round(p["meshes"][0][0][0], 3)) if p["meshes"] else ())
        tri_part = []
        for verts, faces, mat in p["meshes"]:
            stats["src_tris_in_map"] += len(faces)
            if key in seen_models:
                continue
            tri = verts[faces]
            tlo, thi = tri.min(1), tri.max(1)
            keep = np.all(thi >= lo, 1) & np.all(tlo <= hi, 1)
            area = np.linalg.norm(np.cross(tri[:, 1] - tri[:, 0], tri[:, 2] - tri[:, 0]), axis=1)
            keep &= area > 1e-6
            if not keep.any():
                continue
            f = faces[keep]
            used, inv = np.unique(f, return_inverse=True)
            tri_part.append((verts[used], inv.reshape(-1, 3), mat))
        seen_models.add(key)
        if not tri_part:
            continue
        pi = len(P)
        P.append(p)
        ntri = 0
        for verts, f, mat in tri_part:
            v = verts.copy()
            v[:, 0] *= -1  # PSP space
            V.append(v)
            T.append(np.column_stack([f + nv, np.full(len(f), mat), np.full(len(f), pi)]))
            nv += len(v)
            ntri += len(f)
        stats["parts"].append(dict(name=p["name"], cam_row=p["cam_row"], cam_from=p.get("cam_from"), tris=ntri,
                                   materials=sorted({m for _, _, m in tri_part})))
    V = np.concatenate(V).astype(np.float32)
    T = np.concatenate(T).astype(np.int64)
    # weld coincident vertices (HKX subparts repeat them), 1 mm
    stats["verts_before_weld"] = len(V)
    _, first, inv = np.unique(np.round(V / 1e-3).astype(np.int64), axis=0, return_index=True, return_inverse=True)
    V = V[first]
    T[:, :3] = inv.reshape(-1)[T[:, :3]]
    t3 = T[:, :3]
    T = T[(t3[:, 0] != t3[:, 1]) & (t3[:, 1] != t3[:, 2]) & (t3[:, 0] != t3[:, 2])]  # collapsed by the weld
    assert len(V) < 65536 and len(T) < 65536 and len(P) < 256
    # grid over XZ (PSP space: x mirrored)
    cell = cfg["cell"]
    x0, z0 = float(-hi[0]), float(lo[2])
    nx = int(np.ceil((hi[0] - lo[0]) / cell)); nz = int(np.ceil((hi[2] - lo[2]) / cell))
    tri = V[T[:, :3]]
    tlo, thi = tri.min(1), tri.max(1)
    cells = [[] for _ in range(nx * nz)]
    eps = 0.05
    for i in range(len(T)):
        i0 = max(0, int(np.floor((tlo[i, 0] - eps - x0) / cell))); i1 = min(nx - 1, int(np.floor((thi[i, 0] + eps - x0) / cell)))
        j0 = max(0, int(np.floor((tlo[i, 2] - eps - z0) / cell))); j1 = min(nz - 1, int(np.floor((thi[i, 2] + eps - z0) / cell)))
        for j in range(j0, j1 + 1):
            for k in range(i0, i1 + 1):
                cells[j * nx + k].append(i)
    lst, crec = [], []
    for c in cells:
        crec.append((len(lst), len(c)))
        lst += c
    body = bytearray(b"\0" * 80)

    def align():
        while len(body) % 16:
            body.append(0)
    off_vert = len(body); body += V.astype("<f4").tobytes(); align()
    off_tri = len(body)
    for a, b, c, m, pi in T:
        body += struct.pack("<4HBB", a, b, c, m & 0xFFFF, pi, 0)
    align()
    off_part = len(body)
    for p in P:
        body += struct.pack("<16siI", p["name"].encode()[:15], p["cam_row"], 0)
    align()
    off_cell = len(body)
    for s, n in crec:
        body += struct.pack("<2I", s, n)
    align()
    off_list = len(body); body += np.array(lst, "<u2").tobytes(); align()
    body[:80] = struct.pack("<4s7I4f8I", b"PCOL", 1, len(V), len(T), len(P), nx, nz, len(lst),
                            x0, z0, cell, 0.0, off_vert, off_tri, off_part, off_cell, off_list, 0, 0, 0)
    out = Path(args.out).expanduser()
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(body)
    n = np.cross(tri[:, 1] - tri[:, 0], tri[:, 2] - tri[:, 0])
    n /= np.linalg.norm(n, axis=1, keepdims=True)
    counts = [c for _, c in crec]
    stats.update(verts=len(V), tris=len(T), floor_tris=int((n[:, 1] > 0.64).sum()),
                 ceiling_tris=int((n[:, 1] < -0.64).sum()), wall_tris=int((np.abs(n[:, 1]) <= 0.64).sum()),
                 grid=dict(nx=nx, nz=nz, cell=cell, x0=x0, z0=z0, list=len(lst), max_per_cell=max(counts),
                           mean_per_cell=round(float(np.mean(counts)), 1)),
                 bytes=len(body), bounds_psp=dict(min=V.min(0).round(2).tolist(), max=V.max(0).round(2).tolist()),
                 cook_seconds=round(time.time() - t0, 1))
    # spawn: player spot + lock-on target from the MSB (PSP space)
    from soulstruct.darksouls1r.maps import MSB
    import math
    msb = MSB.from_path(DSR / "map" / "MapStudio" / f"{cfg['map']}.msb")
    ch = next(c for c in msb.characters if c.name == cfg["target"])
    tp = [float(v) for v in ch.translate]
    px, py, pz, pyaw = cfg["player"]
    spawn = (f"player {-px:.3f} {py:.3f} {pz:.3f} {-math.radians(pyaw):.4f}\n"
             f"target {ch.model.name} {-tp[0]:.3f} {tp[1]:.3f} {tp[2]:.3f} {-math.radians(float(ch.rotate[1])):.4f}\n")
    out.with_suffix(".spawn").write_text(spawn)
    stats["spawn"] = spawn.strip().split("\n")
    out.with_suffix(".json").write_text(json.dumps(stats, indent=1))
    print(json.dumps(stats, indent=1))


if __name__ == "__main__":
    main()
