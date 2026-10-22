#!/usr/bin/env python3
"""Firelink Shrine (m10_02_00_00) zone pack for the PSP, trimmed to the shrine itself (docs/firelink.md).

    tools/.venv/bin/python tools/cook_firelink.py --out ~/ash-assets/cooked/firelink/zone_m10_02.pak
    tools/.venv/bin/python tools/cook_firelink.py --npc-only --out ...     # only the Crestfallen Warrior model

This is a WRAPPER: tools/cook_world.py, tools/cook_collision.py and tools/cook_knight.py are used as they are
(imported, their functions called) and only overridden from here, because their owners edit them in parallel.
What is overridden and why (each is a candidate to move into the shared cooker, docs/firelink.md §6):
  1. load_piece_batches   map pieces of m10_02 with more than one FLVER bone (foliage, ivy, rocks: m5001..m5064,
                          m4000..) keep vertices in BONE space; cook_world places them as if they were in piece
                          space, so they pile up at the map origin. Here every mesh is moved by its bone's armature
                          transform (mesh.bone_indices[0]) [проверено: m5001B2 bone 0 = (-44.7, -59.1, 56.6), the
                          bush next to the bonfire]. + triangles are clipped to the shrine box (trimmed zone).
  2. config               M18 dict replaced by the m10_02 one (map, area, sky, budget, collision box), OBJ_KIND /
                          ROLE_BY_ENTITY for this map (EMEVD m10_02 + MSB names).
  3. cook_pcol            collision box = the shrine + invisible walls at the four closed exits.
  4. in_box               cook_world cuts "the pit" (m18_01 only); here the same hook cuts everything OUTSIDE the
                          shrine box (objects, characters, regions).
  5. write_ashz           characters reduced to those present at the very start of the game (EMEVD m10_02
                          constructor), RESPAWN param = bonfire 1021960, exit regions added (role OTHER).
  6. write_ashw           (a) far ring: everything outside the shrine box within ~200 m as coarse BF_FAR
                          batches (cliffs, aqueduct, the Burg on the hill), (b) vertex light from
                          tools/cook_light.py (MSB lights + the bonfire) added to the baked lightmap colour,
                          (c) sky texture re-cooked brighter (CLUT8), sky vertex colour 1.0.
  7. EdgeLimit / TexBank  coarser guard-band tessellation (4 m), map textures brightened for the GE.
  Config keys the shared cooker gains later (e.g. quality_near/far) come from cook_world.M18, this zone's on top.
Sidecars written next to the pak (all FromSoftware-derived, never committed):
  zone_m10_02.ashmip   mip chains of the pak's world textures (tools/texmip.py functions), index = WorldTex index
  zone_m10_02.cfg      text: fog (FogBank of the bonfire floor), bonfire / NPC / spawn points for the test EBOOT
  crestfallen.ashc/.ashanim/.json   Crestfallen Warrior (c0000 + CharaInitParam 6270 parts), seated loop a00_7810
"""
from __future__ import annotations

import argparse
import json
import math
import os
import struct
import sys
import time
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).parent))
import cook_world as cw  # noqa: E402
from ashm import Batch, merge_batches, tessellate, weld  # noqa: E402
from ashw import BF_ALPHA, BF_FAR, BF_NOCULL, BF_SKY, ROLES, WBatch  # noqa: E402
from dsrlib import DSR, FLVER, load_map_texture, mesh_arrays, mesh_textures, mesh_triangles, valid_meshes  # noqa: E402

MAP, AREA = "m10_02_00_00", "m10"
# Trimmed zone = the shrine: ruined church with the bonfire, stairs, the plaza, the upper ledge, foot of the
# elevator tower. FromSoft space. [придумал по виду сверху + MSB: bonfire o0200 (-51.8, -60.6, 55.7), start region
# 1022100 (-47.4, -59.8, 53.3), elevator bottom 1022000 (-7.2, -44.7, 106.8)]
KEEP = ((-66.0, -96.0, 36.0), (-12.0, 4.0, 122.0))
FAR_BOX = ((-260.0, -220.0, -150.0), (160.0, 80.0, 330.0))
FAR_BUDGET = 800             # triangles of the far ring [придумал]
FAR_EDGE = 40.0

PB = bool(int(os.environ.get("FL_PB", "0")))   # experiment switch: keep open borders in decimation
PIT = object()               # sentinel for cook_world's cut_pit -> "outside KEEP"

M10 = dict(
    map=MAP, area=AREA,
    cut=("m2022B2",          # water plane in the well (2 tris, A10_Water_lake)
         "m6100B2",          # mist cards under the cliff (A10_mist)
         "m2040B2"),         # graveyard steps beyond the south exit (out of the box anyway)
    sky={"m9000B2": (700, 900.0, BF_SKY)},   # sky dome + cloud ring (r ~2700 m)
    # m2023B2: 15.6k tris of carved stone on the roof ledge (y -30); foliage cards decimate badly -> lower weight
    # grass tufts m5061-m5064 (31k tris of alpha cards around the bonfire): first to go [придумал]
    weights={"m2023B2": 0.2, "m5061B2": 0.15, "m5062B2": 0.15, "m5063B2": 0.15, "m5064B2": 0.15},
    world_budget=30000, max_edge=3.0, chunk=12.0, lod_far_frac=0.5, lod_far_edge=10.0, preserve_border=PB,
    lm_scale=5.5, lm_ambient=0.24,
    # the integrator's per-piece shard/hole boosts (cook_world quality_near/far, uncommitted on 15.10) gave the
    # grass cards of m5061-m5064 x4-x6 triangles and took them from the arcade walls (worse stills) -> off here
    quality_near=(1.0, 1.0), quality_far=(1.0, 1.0),
    col_box=KEEP, col_cell=2.0, col_skip=(),
    underground_y=-1e9, pit_box=PIT,
)
EXTRA_SKIP = ("mist", "Water", "BG_shaft")

# objects of m10_02 [проверено: MSB + EMEVD m10_02]: o0200 bonfire (RegisterBonfire obj 1021960), o1460 the
# elevator to the Undead Parish (EndOfAnimation obj 1021000), o1465 / o6100 the lift and gate down to New Londo
# (1021465 / 1021201, beyond the closed exit), o1480/o1481 the well cover; o0110 chests (宝箱00-03 -> item lots);
# o0020 = 28 "rescue" treasure dummies (宝_救済用, all at one hidden spot) and o0500 corpses -> treasure regions.
OBJ_KIND = {"o0200": ("BONFIRE", 450), "o1460": ("GATE", 300), "o1465": ("GATE", 200), "o6100": ("GATE", 120),
            "o1480": ("DOOR", 120), "o1481": ("DOOR", 120)}
OBJ_SKIP = ("o0500", "o0020", "o1475")      # corpses / dummies: treasure regions only
DECOR_BUDGET = {"o0110": 160, "o1470": 120, "o1495": 40, "o1496": 40, "o1497": 40}
ROLE_BY_ENTITY = {
    1022100: "START",         # "event スタート地点" = spawn point region of SpawnPointEvent 復活イベント00 (1022960)
    1022000: "OTHER", 1022001: "OTHER",   # elevator call regions (bottom) - lift closed in the trimmed zone
}
# Present at the start of the game (EMEVD m10_02 constructor: no DisableCharacter unless a later flag is set)
# [проверено: 6270 disabled only if flag 1464 (hostile/dead); 6080 disabled only if 1192; 6060 never disabled]
START_CHRS = {6270: "Crestfallen Warrior (CharaInitParam 6270 心折れた戦士 拠点), seated a00_7810",
              6080: "Petrus? (CharaInitParam 6080 聖女の騎士A) [версия]",
              6060: "c2750, Anastacia in her cell below the shrine [версия по модели]",
              1020960: "c1000 talk/bonfire proxy at the bonfire (talk 100200)"}
# closed exits: invisible walls (FromSoft space, quad a-b, from y0 to y1) + regions [придумал положение по
# виду сверху коллизии, docs/firelink.md §3]
EXITS = [
    ("EXIT_BURG", 1022901, "к Городу нежити (лестница к акведуку, север)", (-34.0, -52.0, 37.0), (-20.0, -52.0, 37.0)),
    ("EXIT_GRAVE", 1022902, "к кладбищу и Катакомбам (юг)", (-62.0, -70.0, 121.0), (-14.0, -70.0, 121.0)),
    ("EXIT_LONDO", 1022903, "вниз к лифту в Новый Лондо / Нижний город (запад, руины)", (-65.0, -78.0, 40.0),
     (-65.0, -78.0, 70.0)),
    ("EXIT_LIFT", 1022904, "лифт к Церкви нежити (башня, восток)", (-13.0, -50.0, 100.0), (-13.0, -50.0, 116.0)),
]
WALL_H = 40.0
SKY_TEX = {"m10_sky_02": 1.5}                              # brighten (gamma 0.6, gain) [придумал]
SKY_COL = {"m10_sky_02": 1.0, "m10_01_cloud_01": 0.32}   # sky batch vertex colour [придумал]

G: dict = {}   # shared state between the overrides


# --------------------------------------------------------------------------- 1. map pieces with bones + clip

def inside(p, box):
    lo, hi = box
    return np.all(p >= np.array(lo), axis=-1) & np.all(p <= np.array(hi), axis=-1)


def load_piece_batches(cfg, part, flv, bank, lm_cache, skip_mtd=cw.SKIP_MTD, mode="near"):
    """cook_world.load_piece_batches + bone transform per mesh + clip to the shrine box (mode near) or to the far
    ring (mode far: outside KEEP, inside FAR_BOX)."""
    t = np.array([float(v) for v in part.translate], np.float32)
    R = cw.euler_fs(part.rotate).astype(np.float32)
    tree = flv.get_bone_tree().get_bone_armature_space_transforms() if len(flv.bones) > 1 else None
    sky = part.model.name in cfg["sky"]
    by = {}
    for m in valid_meshes(flv):
        mtd = m.material.mat_def_path.rsplit("\\", 1)[-1]
        texs = mesh_textures(m)
        if any(s in mtd for s in tuple(skip_mtd) + EXTRA_SKIP) or not texs.get("g_Diffuse"):
            continue
        d = mesh_arrays(m)
        tri = mesh_triangles(m)
        if len(tri) == 0 or "uv0" not in d:
            continue
        pos = d["pos"].astype(np.float32)
        if tree is not None:
            # per vertex: FLVER bone = mesh.bone_indices[vertex bone index 0] (one mesh holds several instances,
            # e.g. 3 grass tufts on 3 bones) [проверено: m5061B2 mesh.bone_indices (0,1,2), vertex indices 0..2]
            lut = np.array([int(v) for v in m.bone_indices]) if len(m.bone_indices) else np.array([0])
            vb = d["bone_indices"][:, 0] if "bone_indices" in d else np.zeros(len(pos), np.int64)
            gb = lut[np.clip(vb, 0, len(lut) - 1)]
            for bi in np.unique(gb):
                if not 0 <= bi < len(tree):
                    continue
                Rb = np.array(tree[bi][1].data, np.float32).reshape(3, 3)
                sel = gb == bi
                pos[sel] = pos[sel] @ Rb.T + np.array(list(tree[bi][0]), np.float32)
        pos = pos @ R.T + t
        if not sky:
            c = pos[tri].mean(1)
            sel = inside(c, KEEP) if mode == "near" else (~inside(c, KEEP) & inside(c, FAR_BOX))
            tri = tri[sel]
            if len(tri) == 0:
                continue
        dstem = texs["g_Diffuse"]
        ti = bank.add(("map", dstem), cw.map_tex_loader(dstem, ("m10", "m18", "m19")))
        if ti is None:
            continue
        lstem = texs.get("g_Lightmap") if "uv1" in d else None
        if lstem and lstem not in lm_cache:
            r = load_map_texture(cfg["area"], lstem)
            lm_cache[lstem] = None if r is None else np.asarray(r[0].convert("RGB"), np.float32) / 255.0
        if lstem and lm_cache[lstem] is None:
            lstem = None
        carry = np.zeros((len(pos), 3), np.float32)
        if lstem:
            carry[:, :2] = d["uv1"]
        flags = BF_ALPHA if bank.items[ti].alpha else 0
        if "_Alp" in mtd or "_Edge" in mtd or flags:
            flags |= BF_NOCULL
        used, inv = np.unique(tri, return_inverse=True)
        b = Batch(ti, pos[used], d["uv0"][used], carry[used], inv.reshape(-1, 3), flags, f"{part.name}:{dstem}")
        by.setdefault((ti, flags, lstem), []).append(weld(b))
    return [(k, merge_batches(v)) for k, v in by.items()]


class EdgeLimit(cw.EdgeLimit):
    """Coarser guard-band tessellation than cook_world's (3 m near the floor): in the shrine nearly everything is
    within reach of a walkable floor, so 3 m tripled the simplified mesh (11k -> 31k) and left only ~6 % of the
    source for the shapes. 4 m minimum edge, ramp from 3 m off the floor [придумал; holes near the camera checked
    on the bonfire / crestfallen stills]."""

    def __init__(self, pts, e_min=4.0, e_max=40.0, k=1.2, cam_reach=3.0):
        super().__init__(pts, e_min, e_max, k, cam_reach)


# --------------------------------------------------------------------------- 3. collision with closed exits

def wall_parts():
    out = []
    for name, ent, _, a, b in EXITS:
        a, b = np.array(a), np.array(b)
        v = np.array([[a[0], a[1] - 4, a[2]], [b[0], b[1] - 4, b[2]], [b[0], a[1] + WALL_H, b[2]],
                      [a[0], a[1] + WALL_H, a[2]]])
        f = np.array([[0, 1, 2], [0, 2, 3], [0, 2, 1], [0, 3, 2]])   # both sides
        out.append(dict(name=name[:15], model=name, cam_row=-1, meshes=[(v, f, 0)]))
    return out


def cook_pcol(cfg):
    import contextlib
    import io
    import tempfile
    import cook_collision as cc
    lo, hi = cfg["col_box"]
    sp = G["start"]
    cc.SCENES["zone"] = dict(map=cfg["map"], box_min=lo, box_max=hi, cell=cfg["col_cell"], skip=cfg["col_skip"],
                             target="c0000_0018", player=(sp[0], sp[1], sp[2], sp[3]))
    orig = cc.load_parts

    def load_parts_plus(map_id):
        return orig(map_id) + wall_parts()
    cc.load_parts = load_parts_plus
    with tempfile.TemporaryDirectory() as td:
        p = Path(td) / "zone.pcol"
        argv = sys.argv
        sys.argv = ["cook_collision", "--scene", "zone", "--out", str(p)]
        try:
            with contextlib.redirect_stdout(io.StringIO()):
                cc.main()
        finally:
            sys.argv = argv
            cc.load_parts = orig
        data = p.read_bytes()
        G["pcol"] = data
        G["spawn"] = p.with_suffix(".spawn").read_text()
        return data, json.loads(p.with_suffix(".json").read_text())


def pcol_tris(pcol: bytes):
    h = struct.unpack_from("<4s7I4f8I", pcol, 0)
    n_vert, n_tri, off_vert, off_tri = h[2], h[3], h[12], h[13]
    V = np.frombuffer(pcol, "<f4", n_vert * 3, off_vert).reshape(-1, 3)
    T = np.frombuffer(pcol, np.dtype([("v", "<u2", 3), ("m", "<u2"), ("p", "u1"), ("f", "u1")]), n_tri, off_tri)
    return V[T["v"].astype(np.int64)], T["p"]


# --------------------------------------------------------------------------- 4/5. entities

_orig_in_box = cw.in_box


def in_box(p, box):
    if box is PIT:
        return not bool(inside(np.asarray(p, np.float64), KEEP))
    return _orig_in_box(p, box)


_orig_write_ashz = cw.write_ashz


def write_ashz(zone):
    zone.chrs = [c for c in zone.chrs if c["entity"] in START_CHRS]
    # 宝_救済用00-27 (o0020, 28 dummies at one spot, lots 4000-4070 or none) = the "rescue" chest for lost key items,
    # not loot lying in the shrine at the start -> dropped
    zone.regs = [r for r in zone.regs if not (r["role_name"] == "TREASURE" and (r["name"].startswith("o0020")
                                                                               or r["param"] < 0))]
    for r in zone.regs:
        if r["role_name"] == "RESPAWN":
            r["param"] = 1021960          # SpawnPointEvent 復活イベント00 -> bonfire object (RegisterBonfire)
    for name, ent, _, a, b in EXITS:
        a, b = np.array(a), np.array(b)
        c = (a + b) / 2
        zone.regs.append(dict(entity=ent, shape=3, role=ROLES["OTHER"], pos=[-c[0], c[1], c[2]],
                              yaw=-math.atan2(b[0] - a[0], b[2] - a[2]), size=[2.0, 6.0, float(np.linalg.norm(b - a))],
                              param=-2, name=name, role_name="OTHER"))
    G["zone"] = zone
    return _orig_write_ashz(zone)


# --------------------------------------------------------------------------- 6. far ring + vertex light

TEX_GAMMA, TEX_GAIN = 0.72, 1.12   # DS1 map textures are authored for HDR + tone mapping; GE modulate cannot
                                  # brighten above the texel -> brighten at cook [придумал, на глаз]


def brighten(im):
    if im is None:
        return None
    a = np.asarray(im.convert("RGBA")).astype(np.float32)
    a[..., :3] = np.clip(255.0 * (a[..., :3] / 255.0) ** TEX_GAMMA * TEX_GAIN, 0, 255)
    from PIL import Image
    return Image.fromarray(a.astype(np.uint8), "RGBA")


class RecBank(cw.TexBank):
    def __init__(self, *a, **k):
        super().__init__(*a, **k)
        G["bank"] = self

    def add(self, key, loader):
        if key[1] in SKY_TEX or key[1] in SKY_COL:
            return super().add(key, loader)
        return super().add(key, lambda: brighten(loader()))


def far_ring(cfg, bank):
    from soulstruct.darksouls1r.maps import MSB
    msb = MSB.from_path(DSR / "map" / "MapStudio" / f"{MAP}.msb")
    lm_cache: dict = {}
    items = []
    for part in msb.map_pieces:
        model = part.model.name
        if model in cfg["cut"] or model in cfg["sky"]:
            continue
        flv = FLVER.from_path(DSR / "map" / MAP / f"{model}A10.flver.dcx")
        for key, b in load_piece_batches(cfg, part, flv, bank, lm_cache, mode="far"):
            if b.flags & BF_ALPHA:
                continue          # foliage cards far away: noise in 4x4 pixels, skip [придумал]
            items.append((part, key, b))
    src = sum(b.ntri for _, _, b in items)
    out = []
    for part, key, b in items:
        tgt = max(4, int(b.ntri * FAR_BUDGET / max(1, src)))
        for k2, s in cw.simplify_multi([(key, b)], tgt, False):
            s = cw.bake_light(tessellate(s, FAR_EDGE), k2[2], lm_cache, cfg)
            s.pos = s.pos * cw.MIRROR
            out.append(WBatch(s.tex, s.pos, s.uv, s.col, s.tri, BF_FAR, (0xFFFFFFFF,) * 4, f"far:{part.name}"))
    G["far"] = dict(src_tris=int(src), tris=int(sum(len(b.tri) for b in out)), batches=len(out))
    return out


def lights():
    import cook_light as cl
    ls = cl.msb_lights(MAP, "a10")
    bf = G["bonfire_psp"]
    # bonfire: no MSB light event in m10_02 (the flame is the object's own SFX); warm point light [придумал]
    ls.append(cl.Light("bonfire", np.array([bf[0], bf[1] + 0.8, bf[2]], np.float32), 9.0, 1.5,
                       np.array([1.0, 0.62, 0.32], np.float32), 1.6, 1, -1))
    return ls


_orig_write_ashw = cw.write_ashw


def write_ashw(textures, batches, meshes, chunk=12.0):
    import cook_light as cl
    far = far_ring(M10, G["bank"])
    col_tris, _ = pcol_tris(G["pcol"])
    ls = lights()
    G["lights"] = [dict(name=L.name, pos=[round(float(v), 2) for v in L.pos], radius=round(L.radius, 2)) for L in ls]
    lit = 0
    for b in batches:
        if b.flags & (BF_SKY | BF_FAR) or b.flags & 16:    # sky / far / object meshes: no point light
            continue
        add = cl.light_at(b.pos.astype(np.float32), None, ls, col_tris)
        if add.any():
            b.col = np.clip(b.col + add * cl.VERT_GAIN, 0, 1).astype(np.float32)
            lit += 1
    G["lit_batches"] = lit
    # sky: DS1 sky textures are HDR-dark (m10_sky_02 mean RGB 26/26/32) and the cooker gives unlit batches 0.35
    # -> re-cook the sky texture brighter, CLUT8 256 px; vertex colour 1.0; the cloud sea below the cliff dimmed
    # [придумал, на глаз]
    from ashw import cook_texture_world
    for i, t in enumerate(textures):
        key = t.src[0] if t.src else None
        if key and key[1] in SKY_TEX:
            im = cw.map_tex_loader(key[1], ("m10",))()
            a = np.asarray(im.convert("RGBA")).astype(np.float32)
            a[..., :3] = np.clip(255.0 * (a[..., :3] / 255.0) ** 0.6 * SKY_TEX[key[1]], 0, 255)
            from PIL import Image
            nt = cook_texture_world(Image.fromarray(a.astype(np.uint8), "RGBA"), 256, 8)
            nt.src = t.src
            textures[i] = nt
    sky_ti = {i: t.src[0][1] for i, t in enumerate(textures) if t.src and t.src[0][1] in SKY_COL}
    for b in batches:
        if b.flags & BF_SKY and b.tex in sky_ti:
            b.col = np.full_like(b.col, SKY_COL[sky_ti[b.tex]])
    n = len(far)
    return _orig_write_ashw(textures, far + list(batches), [(s + n, c, r) for s, c, r in meshes], chunk)


# --------------------------------------------------------------------------- sidecars

def write_mips(pak: Path):
    """.ashmip for the world textures inside the pak (WorldTex index order)."""
    import texmip
    data = pak.read_bytes()
    n = struct.unpack_from("<4s3I", data, 0)[2]
    ents = {struct.unpack_from("<16s", data, 16 + 32 * i)[0].split(b"\0")[0].decode():
            struct.unpack_from("<2I", data, 16 + 32 * i + 16) for i in range(n)}
    off, _ = ents["world"]
    n_tex = struct.unpack_from("<I", data, off + 8)[0]
    off_tex = struct.unpack_from("<I", data, off + 32)[0]
    texs = []
    for i in range(n_tex):
        w, h, psm, fl, oc, op = struct.unpack_from("<4H2I", data, off + off_tex + 16 * i)
        nc = 16 if psm == 4 else 256
        clut = np.frombuffer(data, np.uint8, nc * 4, off + oc).reshape(nc, 4)
        size = (w // 2 if psm == 4 else w) * h
        idx = texmip.unswizzle(data[off + op: off + op + size], w, h, psm)
        texs.append((w, h, psm, texmip.mip_levels(idx, clut, psm, 4)))
    return texmip.write_ashmip(pak.with_suffix(".ashmip"), texs)


def write_cfg(pak: Path, cfg_extra: dict):
    import cook_light as cl
    fog = cl.zone_fog(MAP, "a10", G["bonfire_floor"])
    z = G["zone"]
    lines = [f"fog {fog['near']:.1f} {fog['far']:.1f} {fog['color']:08x} {fog['clear']:08x}"]
    bf = G["bonfire_psp"]
    lines.append(f"bonfire {bf[0]:.3f} {bf[1]:.3f} {bf[2]:.3f}")
    for c in z.chrs:
        lines.append(f"chr {c['model']} {c['entity']} {c['pos'][0]:.3f} {c['pos'][1]:.3f} {c['pos'][2]:.3f} "
                     f"{c['yaw']:.4f}")
    for line in G["spawn"].strip().split("\n"):
        lines.append(line)
    pak.with_suffix(".cfg").write_text("\n".join(lines) + "\n")
    return fog


# --------------------------------------------------------------------------- NPC

CRESTFALLEN_PARTS = (("FC_M_0000", 0.06), ("HR_M_0000", 0.03), ("BD_M_9300", 0.40), ("AM_M_9300", 0.18),
                     ("LG_M_9300", 0.22), ("WP_A_0201", 0.04), ("WP_A_1515", 0.07))


def cook_npc(out_dir: Path, budget=2000):
    """Crestfallen Warrior: c0000 + CharaInitParam 6270 equipment [проверено: Body 171008 / Arms 172008 /
    Legs 173008 -> EquipParamProtector model 9300 (Chain armour set, Brigand gloves), R 201010 -> WP 0201
    (Longsword), L 1450010 -> WP 1515 (Tower kite shield), no helmet -> face/hair FC_M_0000/HR_M_0000 [версия:
    real face = FaceGen 6270, not cooked]]. cook_knight with PARTS/CLIPS swapped; the clip is the MSB default
    animation of c0000_0018 = 7810 (a00_7810, seated)."""
    import shutil
    import tempfile
    import cook_knight as ck
    old = ck.PARTS, ck.CLIPS
    ck.PARTS = CRESTFALLEN_PARTS
    ck.CLIPS = [("sit", "a00_7810", True), ("idle", "a00_0000", True)]
    td = Path(tempfile.mkdtemp(prefix="npc-", dir=str(out_dir)))
    argv = sys.argv
    sys.argv = ["cook_knight", "--out-dir", str(td), "--budget", str(budget)]
    try:
        ck.main()
    except SystemExit as ex:      # cook_knight exits with the unit-test status
        if ex.code not in (None, 0):
            raise
    finally:
        sys.argv = argv
        ck.PARTS, ck.CLIPS = old
    for ext in ("ashc", "ashanim", "json"):
        src = td / f"knight.{ext}"
        if src.exists():
            shutil.move(str(src), str(out_dir / f"crestfallen.{ext}"))
    for p in td.glob("*"):
        p.unlink()
    td.rmdir()


# --------------------------------------------------------------------------- main

def prepare():
    from soulstruct.darksouls1r.maps import MSB
    msb = MSB.from_path(DSR / "map" / "MapStudio" / f"{MAP}.msb")
    st = next(r for r in msb.regions if r.entity_id == 1022100)
    G["start"] = ([float(v) for v in st.translate][0], float(st.translate[1]), float(st.translate[2]),
                  float(st.rotate[1]))
    bf = next(o for o in msb.objects if o.entity_id == 1021960)
    G["bonfire_psp"] = [-float(bf.translate[0]), float(bf.translate[1]), float(bf.translate[2])]
    G["bonfire_floor"] = "h0001B2_0000"


def main():
    if cw.GLSHIM.is_dir() and str(cw.GLSHIM) not in os.environ.get("LD_LIBRARY_PATH", ""):
        env = dict(os.environ, LD_LIBRARY_PATH=f"{cw.GLSHIM}:{os.environ.get('LD_LIBRARY_PATH', '')}")
        os.execve(sys.executable, [sys.executable] + sys.argv, env)
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--budget", type=int, default=None)
    ap.add_argument("--npc-only", action="store_true")
    ap.add_argument("--no-npc", action="store_true")
    ap.add_argument("--max-pak-mb", type=float, default=9.0)
    ap.add_argument("--lod-far-frac", type=float, default=None)
    ap.add_argument("--far-budget", type=int, default=None)
    a = ap.parse_args()
    out = Path(a.out).expanduser()
    out.parent.mkdir(parents=True, exist_ok=True)
    t0 = time.time()
    if not a.npc_only:
        prepare()
        # keys the shared cooker gained since (e.g. quality_near/far of the integrator's shard/hole check) keep the
        # cook_world defaults; everything this zone sets explicitly wins
        cfg = {**dict(cw.M18), **M10}
        if a.budget:
            cfg["world_budget"] = a.budget
        if a.lod_far_frac:
            cfg["lod_far_frac"] = a.lod_far_frac
        if a.far_budget:
            global FAR_BUDGET
            FAR_BUDGET = a.far_budget
        cw.M18.clear(); cw.M18.update(cfg)
        cw.OBJ_KIND.clear(); cw.OBJ_KIND.update(OBJ_KIND)
        cw.DECOR_BUDGET.clear(); cw.DECOR_BUDGET.update(DECOR_BUDGET)
        cw.OBJ_SKIP = OBJ_SKIP
        cw.ROLE_BY_ENTITY.clear(); cw.ROLE_BY_ENTITY.update(ROLE_BY_ENTITY)
        cw.FLOOR_PLATES = ()
        cw.load_piece_batches = load_piece_batches
        cw.cook_pcol = cook_pcol
        cw.EdgeLimit = EdgeLimit
        cw.in_box = in_box
        cw.write_ashz = write_ashz
        cw.write_ashw = write_ashw
        cw.TexBank = RecBank
        argv = sys.argv
        sys.argv = ["cook_world", "--out", str(out), "--max-pak-mb", str(a.max_pak_mb)]
        try:
            cw.main()
        finally:
            sys.argv = argv
        mips = write_mips(out)
        fog = write_cfg(out, {})
        js = out.with_suffix(".json")
        st = json.loads(js.read_text())
        st["firelink"] = dict(keep_box=KEEP, far=G.get("far"), lights=G.get("lights"), lit_batches=G.get("lit_batches"),
                              mips=mips, fog=fog, exits=[dict(name=n, entity=e, what=w) for n, e, w, _, _ in EXITS],
                              start_chrs={str(k): v for k, v in START_CHRS.items()},
                              pak_plus_mips_mb=round((out.stat().st_size + mips["bytes"]) / 2**20, 2))
        js.write_text(json.dumps(st, indent=1, default=str, ensure_ascii=False))
        print(json.dumps(st["firelink"], indent=1, default=str, ensure_ascii=False))
    if not a.no_npc:
        cook_npc(out.parent)
    print(f"cook_firelink: {time.time() - t0:.0f} s")


if __name__ == "__main__":
    main()
