#!/usr/bin/env python3
"""Cook a whole DSR map into one PSP zone package `zone_<map>.pak` (docs/world.md).

    tools/.venv/bin/python tools/cook_world.py --out ~/ash-assets/cooked/world/zone_m18_01.pak

What goes in (tools/ashw.py has the byte layout):
  "world"  .ashw  every map piece of the MSB (minus the cut list), simplified to a global triangle budget, lightmap
                  baked into vertex colour after simplification, split into 12 m chunks, one batch per
                  (chunk, texture, MSB draw-group mask); sky/backdrop pieces as BF_SKY / BF_FAR batches;
                  static MSB objects (jars, gargoyles, pillars, nest...) baked into the world; models of the dynamic
                  objects (doors, cell doors, gate, bonfire, ladders, fog walls...) as BF_MESH batches.
                  Textures CLUT4 (16 colours, <= 128 px) by default, 12-byte vertices (u16 uv, RGB565, s16 xyz).
  "pcol"   .pcol  Havok hi-res collision of the playable part of the map (tools/cook_collision.py format v1,
                  produced by that cooker with a whole-map box; underground pit cut).
  "ents"   .ashz  collisions with their display/draw groups, objects (kind, MSB entity id, transform, HKX boxes),
                  characters (model, entity, think/npc params, patrol), regions with roles (start, respawn, boss
                  triggers, demon drop point, plunge balcony, fog entry, crow exit, messages, torches, treasure).

Roles / kinds come from the MSB and the map's EMEVD (m18_01_00_00.emevd, decompiled with soulstruct) [проверено]:
the Asylum Demon of the first fight is entity 1810800 = model c2232 (EnableBossHealthBar(1810800, name=2232)),
c2230 = entity 1810810 is the second demon in the underground pit (name 2231), which is out of slice S1.
Never committed: everything this writes is FromSoftware data.
"""
from __future__ import annotations

import argparse
import json
import logging
import math
import sys
import time
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).parent))
logging.disable(logging.WARNING)
from ashm import Batch, simplify, tessellate, weld, merge_batches  # noqa: E402
from ashw import (BF_ADD, BF_ALPHA, BF_BLEND, BF_DECOR, BF_FAR, BF_LOD_FAR, BF_LOD_NEAR, BF_MESH, BF_NOCULL, BF_SKY, CHR_BOSS, CHR_CORE, KINDS, ROLES, SHAPES,  # noqa: E402
                  WBatch, Zone, cook_texture_soft, cook_texture_world, mask128, write_ashw, write_ashz, write_pak)
from dsrlib import (DSR, FLVER, Binder, binder_flver, binder_textures, load_map_texture, mesh_arrays,  # noqa: E402
                    mesh_textures, mesh_triangles, valid_meshes)

SKIP_MTD = ("Water", "_Edge", "Fly", "insect", "Snow", "Light", "light_shaft", "_shaft", "Shadow", "Mirror")
# helper planes with 16x16 placeholder textures (m0050/m0100: m18_ao, m0010/m0062: m18_lit_black, plain M[D]): drawn
# opaque they were flat grey slabs over the yard floor in front of the big door [версия: служебная геометрия света,
# в DSR не видна; по скринам pt1-death fr_0605/0610]
SKIP_DIFFUSE = ("m18_ao", "m18_lit_black")
MIRROR = np.array([-1.0, 1.0, 1.0], np.float32)

M18 = dict(
    map="m18_01_00_00", area="m18",
    # underground pit of the second demon (c2230, not in slice S1) and helper pieces
    cut=("m0301B1", "m0310B1", "m0320B1", "m9999B1", "m0120B1",
         "m5001B1"),   # m5001: 539-bone rubble of the wall the demon breaks (floats in bind pose)
    sky={  # piece: (triangle budget, max edge m, flags)
        "m9100B1": (500, 700.0, BF_SKY),       # sky dome (r ~2900 m)
        "m9000B1": (700, 500.0, BF_SKY),       # mountains + cloud ring (r ~1570 m)
        "m5201B1": (200, 150.0, BF_SKY),       # cloud sheets
        "m5200B1": (900, 30.0, BF_FAR),        # distant cliffs around the asylum (300 m)
    },
    # m5000/m5101/m5010: cliff path to the crow, walked on and seen up close (was 0.6: spikes on the path);
    # its frames are the cheapest of the zone (~4-5k triangles), the global ratio takes it back elsewhere
    # loose stones the quadric simplifier melts into floating pyramids (g_Diffuse per model): kept whole
    # m5000 mesh 15 = the stepping stones of the cliff path (1485 tris, 134 separate stones) [проверено: офлайн-рендер
    # tools/world_render.py исходника против кука, shots wfix2-*]
    keep_whole={"m5000B1": ("m19_stairs_10_snow",)},
    # stairs of every piece: walked on and seen from 1-3 m; simplified they became ramps/slabs (the steps up to the big
    # door of the hall: one dark box over the stairs). All of them ~5k tris before tessellation [проверено: 16.10]
    keep_whole_any=("m19_b_stairs_00", "m19_b_stairs_01", "m19_staircase_10", "m19_stairs_10"),
    # loose stone blocks (merlons on the battlements, ruins on the cliff and the balcony pillars: m19_wall_13 /
    # m19_wall_11, 10-12 tris a block, ~5k blocks): a box cannot be simplified, the quadric simplifier melted them into
    # floating diamonds/slabs (upper yard, cliff path, east balcony). Since 16.10 the blocks of a piece are fused into one
    # solid (blocks_union) simplified to ratio * blocks_w of their triangles (far LOD: its own ratio) [урезание: камни
    # сливаются в массив]. All whole: +58k tris near, frames x2 (world_stats, 15.10) - not affordable [проверено]
    blocks=("m19_wall_13", "m19_wall_11"), blocks_w=2.5,
    # far LOD share of the near LOD per model: the cliff rocks seen at 8-15 m from the path (far LOD from 8 m) were big
    # flat moss slabs at 0.22 (wfix3-boss f8070-8130 [проверено: world_render --lod 1000 без них]); cheap frames there
    far_frac={"m5000B1": 0.6, "m5101B1": 0.6, "m5100B1": 0.6, "m5010B1": 0.6},
    weights={"m5000B1": 3.0, "m5001B1": 0.45, "m5101B1": 2.5, "m5100B1": 2.0, "m5010B1": 2.0,
             "m0045B1": 0.8, "m0060B1": 0.8},
    world_budget=86000, max_edge=3.0, chunk=12.0, lod_far_frac=0.22, lod_far_edge=6.0, preserve_border=False,
    # max (shard, hole) area fractions near the walkable floor, geom_error(); the cook boosts a piece until it holds
    quality_near=(0.04, 0.10), quality_far=(0.10, 0.28),
    lm_scale=3.8, lm_ambient=0.16,
    # collision: playable box (FromSoft space) and cut parts (underground pit)
    col_box=((-34.0, 172.0, -106.0), (52.0, 240.0, 36.0)), col_cell=2.0,
    col_skip=("h0016B1", "h1600B1", "h1601B1", "h1699B1", "h1699B1_0000"),
    underground_y=186.0,     # objects / characters of the pit below this height and inside the pit box are cut
    pit_box=((-14.0, 176.0, -33.0), (24.0, 186.0, -3.0)),
)

# MSB object model -> (kind, triangle budget). Kinds from the EMEVD of m18_01 [проверено]:
#   o0200 bonfire (RegisterBonfire 1811960/1811961), o8540 cell doors (ObjAct 牢屋扉), o8541/o8544 church doors
#   (ObjAct 教会の扉), o8542 lattice gate 1811115 (EndOfAnimation after the boss), o8950/o8952/o8953 fog walls
#   (VFX 光の壁 attached, collision box only), o8500/o8501 ladders (RegisterLadder 1811140), o8510/o8511 floor that
#   collapses into the pit (region 床が崩れる判定), o8530 iron ball + o8520/o8521 the wall it breaks (鉄球), o8550 crow
#   nest (カラスの巣). Jars/bags/gargoyles/pillars: static decor baked into the world (urezanie: not breakable).
OBJ_KIND = {
    "o0200": ("BONFIRE", 450), "o8540": ("CELL_DOOR", 400), "o8541": ("DOOR", 200), "o8544": ("DOOR", 200),
    "o8542": ("GATE", 400), "o8950": ("FOG", 0), "o8952": ("FOG", 0), "o8953": ("FOG", 0),
    "o8500": ("LADDER", 160), "o8501": ("LADDER", 160), "o8510": ("FLOOR_BREAK", 500), "o8511": ("FLOOR_BREAK", 60),
    "o8530": ("BOULDER", 250), "o8520": ("BREAKABLE", 150), "o8521": ("BREAKABLE", 40),
}
DECOR_BUDGET = {"o8400": 24, "o8401": 24, "o8402": 36, "o8403": 36, "o8410": 36, "o8411": 44, "o8420": 110,
                "o8421": 110, "o8422": 110, "o8425": 110, "o8600": 110, "o8610": 60, "o8550": 700, "o8700": 260,
                "o8701": 100, "o8702": 100}
OBJ_SKIP = ("o0500", "o8549", "o8531")   # treasure corpses (-> treasure regions), rats, ball path helper

# region roles by MSB name / entity [проверено MSB names + EMEVD]
ROLE_BY_ENTITY = {
    1812011: "START",            # PlayCutscene(180101, move_to_region=1812011): where the player wakes up
    1812100: "RESPAWN",          # SetRespawnPoint(1812900) -> spawn point region "event ゲームスタート地点"
    1812010: "CELL", 1812990: "BOSS_ARENA", 1812996: "BOSS_START", 1812998: "BOSS_ENTRY", 1812305: "DEMON_DROP",
    1812300: "DEMON_LAND", 1812301: "DEMON_BALCONY", 1812350: "PLUNGE_ZONE", 1812351: "PLUNGE_BALCONY_IN",
    1812352: "PLUNGE_BALCONY_IN", 1812320: "BOSS_ESCAPE", 1812000: "EXIT_CROW", 1812001: "CROW_NEST",
    1812400: "FLOOR_COLLAPSE", 1812120: "BOULDER_TRIGGER", 1812020: "INNER_DOOR", 1812021: "INNER_DOOR",
}
ROLE_BY_NAME = [("篝火復活ポイント", "RESPAWN"), ("チュートリアルメッセージ", "MESSAGE"), ("松明", "TORCH"),
                ("巡回", "PATROL"), ("BGM", "BGM"), ("環境ライトスポット", "LIGHT"), ("光の壁", "FOG_VFX"),
                ("SFX", "FOG_VFX")]


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


def euler_fs(deg) -> np.ndarray:
    """MSB rotate (x, y, z deg), R = Ry * Rz * Rx (same as tools/cook_collision.py) [версия: almost all are pure Y]."""
    x, y, z = np.radians([float(v) for v in deg])
    cx, sx, cy, sy, cz, sz = np.cos(x), np.sin(x), np.cos(y), np.sin(y), np.cos(z), np.sin(z)
    Rx = np.array([[1, 0, 0], [0, cx, -sx], [0, sx, cx]])
    Ry = np.array([[cy, 0, sy], [0, 1, 0], [-sy, 0, cy]])
    Rz = np.array([[cz, -sz, 0], [sz, cz, 0], [0, 0, 1]])
    return Ry @ Rz @ Rx


def to_psp_rot(R: np.ndarray) -> np.ndarray:
    M = np.diag(MIRROR.astype(np.float64))
    return M @ R @ M


def bits(bs) -> list[int]:
    return sorted(bs.enabled_bits)


class TexBank:
    def __init__(self, max_size=128, bits=4):
        self.items, self.index = [], {}
        self.max_size, self.bits = max_size, bits

    def add(self, key, loader, soft=False):
        if key in self.index:
            return self.index[key]
        im = loader()
        if im is None:
            return None
        t = (cook_texture_soft if soft else cook_texture_world)(im, self.max_size, self.bits)
        t.src = (key, im.size)
        self.index[key] = len(self.items)
        self.items.append(t)
        return self.index[key]


def map_tex_loader(stem, areas=("m18", "m19", "m10")):
    def f():
        for a in areas:
            r = load_map_texture(a, stem)
            if r is not None:
                return r[0]
        return None
    return f


def split_chunks(b: Batch, chunk: float) -> list[Batch]:
    if b.ntri == 0:
        return []
    c = b.pos[b.tri].mean(1)
    key = np.floor(c / chunk).astype(np.int64)
    _, inv = np.unique(key, axis=0, return_inverse=True)
    inv = inv.reshape(-1)
    out = []
    for k in range(inv.max() + 1):
        t = b.tri[inv == k]
        used, ti = np.unique(t, return_inverse=True)
        out.append(Batch(b.tex, b.pos[used], b.uv[used], b.col[used], ti.reshape(-1, 3), b.flags, b.name))
    return out


def split_u16(b: Batch, limit=60000) -> list[Batch]:
    if len(b.pos) <= limit:
        return [b]
    out = []
    step = max(1, len(b.tri) * limit // (len(b.pos) * 3 + 1))
    for i in range(0, len(b.tri), step):
        t = b.tri[i:i + step]
        used, inv = np.unique(t, return_inverse=True)
        out.append(Batch(b.tex, b.pos[used], b.uv[used], b.col[used], inv.reshape(-1, 3), b.flags, b.name))
    return out


class EdgeLimit:
    """Max triangle edge as a function of how close the camera can get (PSP GE culls triangles whose projection
    leaves the guard band, so only geometry near the player needs small triangles). Camera positions ~ walkable
    collision floor + 1.6 m, the camera orbits <= `cam_reach` m from the player:
        max_edge = clamp(k * (dist_to_walkable - cam_reach), e_min, e_max)                       [придумал k, reach]"""

    def __init__(self, floor_pts_fs: np.ndarray, e_min=3.0, e_max=40.0, k=1.5, cam_reach=4.5):
        from scipy.spatial import cKDTree
        self.tree = cKDTree(floor_pts_fs)
        self.e_min, self.e_max, self.k, self.reach = e_min, e_max, k, cam_reach

    def __call__(self, pts: np.ndarray) -> np.ndarray:
        d, _ = self.tree.query(pts, k=1)
        return np.clip(self.k * (d - self.reach), self.e_min, self.e_max)


def tessellate_adaptive(b: Batch, limit) -> Batch:
    """Longest-edge bisection until every triangle's longest edge <= limit(closest point of the triangle)."""
    P = b.pos[b.tri]; U = b.uv[b.tri]; Cc = b.col[b.tri]
    out_p, out_u, out_c = [], [], []
    for _ in range(14):
        e = np.stack([np.linalg.norm(P[:, 1] - P[:, 0], axis=1), np.linalg.norm(P[:, 2] - P[:, 1], axis=1),
                      np.linalg.norm(P[:, 0] - P[:, 2], axis=1)], 1)
        # distance of the triangle to the camera space ~ min over its corners and centroid
        lim = np.minimum.reduce([limit(P[:, 0]), limit(P[:, 1]), limit(P[:, 2]), limit(P.mean(1))])
        big = e.max(1) > lim
        out_p.append(P[~big]); out_u.append(U[~big]); out_c.append(Cc[~big])
        if not big.any():
            break
        P, U, Cc, e = P[big], U[big], Cc[big], e[big]
        k = e.argmax(1)
        idx = np.stack([k, (k + 1) % 3, (k + 2) % 3], 1)
        r = np.arange(len(P))[:, None]
        P, U, Cc = P[r, idx], U[r, idx], Cc[r, idx]
        mp, mu, mc = (P[:, 0] + P[:, 1]) / 2, (U[:, 0] + U[:, 1]) / 2, (Cc[:, 0] + Cc[:, 1]) / 2
        P = np.concatenate([np.stack([P[:, 0], mp, P[:, 2]], 1), np.stack([mp, P[:, 1], P[:, 2]], 1)])
        U = np.concatenate([np.stack([U[:, 0], mu, U[:, 2]], 1), np.stack([mu, U[:, 1], U[:, 2]], 1)])
        Cc = np.concatenate([np.stack([Cc[:, 0], mc, Cc[:, 2]], 1), np.stack([mc, Cc[:, 1], Cc[:, 2]], 1)])
    else:
        out_p.append(P); out_u.append(U); out_c.append(Cc)
    P = np.concatenate(out_p); U = np.concatenate(out_u); Cc = np.concatenate(out_c)
    M = len(P)
    return weld(Batch(b.tex, P.reshape(-1, 3), U.reshape(-1, 2), Cc.reshape(-1, 3), np.arange(M * 3).reshape(M, 3),
                      b.flags, b.name))


def walkable_points(pcol: bytes, step=1.5) -> np.ndarray:
    """Floor triangles of a .pcol (normal.y > 0.64) sampled every ~step m, +1.6 m, back in FromSoft space."""
    import struct
    h = struct.unpack_from("<4s7I4f8I", pcol, 0)
    n_vert, n_tri = h[2], h[3]
    off_vert, off_tri = h[12], h[13]
    V = np.frombuffer(pcol, "<f4", n_vert * 3, off_vert).reshape(-1, 3)
    T = np.frombuffer(pcol, np.dtype([("v", "<u2", 3), ("m", "<u2"), ("p", "u1"), ("f", "u1")]), n_tri, off_tri)["v"]
    P = V[T.astype(np.int64)]
    n = np.cross(P[:, 1] - P[:, 0], P[:, 2] - P[:, 0])
    ln = np.linalg.norm(n, axis=1)
    up = n[:, 1] / (ln + 1e-12) > 0.64
    P = P[up]
    pts = [P.mean(1)]
    for _ in range(4):   # sample big floor triangles too (midpoints of a few subdivisions)
        e = np.linalg.norm(P[:, 1] - P[:, 0], axis=1).clip(min=np.linalg.norm(P[:, 2] - P[:, 1], axis=1))
        big = e > step
        if not big.any():
            break
        P = P[big]
        m01, m12, m20 = (P[:, 0] + P[:, 1]) / 2, (P[:, 1] + P[:, 2]) / 2, (P[:, 2] + P[:, 0]) / 2
        P = np.concatenate([np.stack([P[:, 0], m01, m20], 1), np.stack([m01, P[:, 1], m12], 1),
                            np.stack([m20, m12, P[:, 2]], 1), np.stack([m01, m12, m20], 1)])
        pts.append(P.mean(1))
    pts = np.concatenate(pts) + [0, 1.6, 0]
    return (pts * MIRROR).astype(np.float32)


# --------------------------------------------------------------------------- map pieces

def diffuse_layers(m, d: dict, texs: dict, tri: np.ndarray) -> list:
    """-> [(diffuse stem, uv (N,2), triangles)] of one FLVER mesh. Multi-layer materials ([M] in the MTD: g_Diffuse +
    g_Diffuse_2 on UV0 / UV1, blended by vertex colour alpha, alpha 1 = g_Diffuse_2 [проверено по soulstruct-blender
    darksouls1r node_tree_builder: Mix(Factor=vertex alpha, A=DSB 0, B=DSB 1)]) are split per triangle by the mean
    alpha into the dominant layer (the GE draws one texture per pass) [урезание: no soft blend between the layers].
    Floors of the Asylum are mostly such materials (wall stone + floor slabs); before this only g_Diffuse was drawn."""
    d2 = texs.get("g_Diffuse_2")
    if not d2 or "color" not in d or "uv1" not in d:
        return [(texs["g_Diffuse"], d["uv0"], tri)]
    a = d["color"][:, 3][tri].mean(1)
    out = []
    for stem, uv, sel in ((texs["g_Diffuse"], d["uv0"], a < 0.5), (d2, d["uv1"], a >= 0.5)):
        if sel.any():
            out.append((stem, uv, tri[sel]))
    return out


def alp_keep(alpha: np.ndarray, tri: np.ndarray, opaque=0.95, solid_share=0.5, trim=True) -> np.ndarray:
    """Triangles of an `_Alp` (alpha-blended) map mesh that the PSP can draw. DS1 blends these by VERTEX alpha; the
    world vertex is RGB565 without alpha, so drawn they were opaque. Most are blend layers over a wall/floor that is
    already there: plaster/dirt over masonry (m19_b_wall_02/05/07, m19_b_wall_02_dirty, A_19_M..._Alp_Spec), snow
    patches on the cliff path (m19_snow_11), the cloud ring and cloud sheets of the sky (m19_bg_cloud_10 in
    m9000/m5201): about half of their triangles have mean alpha < 0.5 [проверено: разбор вершинной альфы всех _Alp
    мешей m18_01]. Opaque and simplified on their own they were the white/pale "shards" on the cliff path and in the
    east wing and the noisy grey band in the sky (pt2-boss cliff-shards, pt2-route eastwing-shards, sky-noise) ->
    dropped whole [урезание: no wall dirt / snow patches / cloud ring]. A mesh that is mostly opaque (rubble,
    break_block, blood decals with texture alpha: >= solid_share of triangles with alpha > `opaque`) keeps its
    triangles with mean alpha >= 0.5 (the faded rim goes); trim=False keeps them all (blended sky clouds: cut
    triangles showed as straight edges in the cloud layer)."""
    a = alpha[tri].mean(1)
    if (a > opaque).mean() < solid_share:
        return tri[:0]
    return tri[a >= 0.5] if trim else tri


_BONE_ARM: dict = {}   # id(FLVER) -> (FLVER, [(R*S, T) per bone])


def bone_pose(flv, m, pos: np.ndarray) -> np.ndarray:
    """Vertices of a rigid multi-bone map mesh in piece space. A few map pieces are built from bones (m0040B1: 20
    bones - the steps up to the big door of the hall, the merlon blocks, grass; m5001B1: 539): each vertex lies in the
    space of its bone (vertex bone_indices[0] -> mesh.bone_indices -> FLVER bone), the bone's armature transform
    (T * R * S) puts it in place. Without it m0040 sat at the piece origin = the yard in front of the hall door: no steps
    there (a black hole down to the hall interior) and its wall blocks floating as pale cubes (pt3/route f1815)
    [проверено: ступени после позы x 0,9..5,5, y 196..198, z -2,1..1,3 (FS) = ступени коллизии]."""
    if len(flv.bones) < 2 or "bone_indices" not in m.vertices.dtype.names or not len(m.bone_indices):
        return pos
    arm = _BONE_ARM.get(id(flv))
    if arm is None or arm[0] is not flv:
        mats = []
        for t, r, s in flv.get_bone_tree().get_bone_armature_space_transforms():
            R = np.array(r.data if hasattr(r, "data") else r, np.float64).reshape(3, 3)
            mats.append((R * np.array(list(s), np.float64)[None, :], np.array(list(t), np.float64)))
        arm = _BONE_ARM[id(flv)] = (flv, mats)
    lb = np.asarray(m.vertices["bone_indices"])[:, 0]
    table = np.asarray(m.bone_indices)
    gb = table[np.clip(lb, 0, len(table) - 1)]
    out = np.empty_like(pos)
    for b in np.unique(gb):
        sel = gb == b
        RS, T = arm[1][b]
        out[sel] = (pos[sel].astype(np.float64) @ RS.T + T).astype(pos.dtype)
    return out


def lightmap_uv(m, d: dict, texs: dict):
    """Lightmap UV: the layer after the diffuse UVs (UV1, or UV2 for multi-layer materials, soulstruct UVLayer)."""
    if texs.get("g_Diffuse_2") and "uv_2" in m.vertices.dtype.names:
        return np.asarray(m.vertices["uv_2"], np.float32)
    if texs.get("g_Diffuse_2"):
        return None
    return d.get("uv1")


def load_piece_batches(cfg, part, flv, bank: TexBank, lm_cache: dict, skip_mtd=SKIP_MTD):
    """-> list of (key, welded Batch in FS world space, colour slot carries the lightmap UV) for one MSB map piece.
    Meshes whose g_Diffuse is in cfg keep_whole[model] get the key (ti, flags, lstem, "whole"): simplify_multi passes them
    through untouched; `blocks` meshes (loose stone blocks, see M18["blocks"]) get "blocks", minus their bottom faces.
    Sky pieces: `_Alp` materials (clouds) keep soft texture alpha and are alpha-blended (BF_BLEND)."""
    t = np.array([float(v) for v in part.translate], np.float32)
    R = euler_fs(part.rotate).astype(np.float32)
    model = part.model.name
    keep_whole = cfg.get("keep_whole", {}).get(model, ())
    sky = model in cfg["sky"]
    blocks = cfg.get("blocks", ())
    by = {}
    for m in valid_meshes(flv):
        mtd = m.material.mat_def_path.rsplit("\\", 1)[-1]
        texs = mesh_textures(m)
        if any(s in mtd for s in skip_mtd) or not texs.get("g_Diffuse") or texs["g_Diffuse"] in SKIP_DIFFUSE:
            continue
        d = mesh_arrays(m)
        tri = mesh_triangles(m)
        if len(tri) == 0 or "uv0" not in d:
            continue
        luv = lightmap_uv(m, d, texs)
        lstem = texs.get("g_Lightmap") if luv is not None else None
        if lstem and lstem not in lm_cache:
            r = load_map_texture(cfg["area"], lstem)
            lm_cache[lstem] = None if r is None else np.asarray(r[0].convert("RGB"), np.float32) / 255.0
        if lstem and lm_cache[lstem] is None:
            lstem = None
        carry = np.zeros((len(d["pos"]), 3), np.float32)
        if lstem:
            carry[:, :2] = luv
        d["pos"] = bone_pose(flv, m, d["pos"])
        pos = d["pos"] @ R.T + t
        if "_Alp" in mtd and "color" in d:
            tri = alp_keep(d["color"][:, 3], tri, trim=not sky)
            if len(tri) == 0:
                continue
        whole = "whole" if texs["g_Diffuse"] in keep_whole else None
        if whole is None and texs["g_Diffuse"] in cfg.get("keep_whole_any", ()):
            whole = "stairs"   # whole in the near LOD only (see simplify_multi lod_far)
        if not sky and (texs["g_Diffuse"] in blocks or texs.get("g_Diffuse_2") in blocks):   # block layer of [M]
            P = d["pos"][tri]
            nrm = np.cross(P[:, 1] - P[:, 0], P[:, 2] - P[:, 0])
            # bottoms sit on the wall: unseen. FromSoft winding is CW from the front (right-hand normal points INTO the
            # block), so a bottom has its right-hand normal UP; until 16.10 this kept "> -0.7" and dropped the TOPS
            tri = tri[nrm[:, 1] / (np.linalg.norm(nrm, axis=1) + 1e-12) < 0.7]
            whole = "blocks"
        soft = sky and "_Alp" in mtd
        for dstem, uv, ltri in diffuse_layers(m, d, texs, tri):
            ti = bank.add(("map", dstem, "soft") if soft else ("map", dstem), map_tex_loader(dstem), soft=soft)
            if ti is None:
                continue
            flags = BF_BLEND if soft else (BF_ALPHA if bank.items[ti].alpha else 0)
            if "_Alp" in mtd or flags:
                flags |= BF_NOCULL
            used, li = np.unique(ltri, return_inverse=True)
            by.setdefault((ti, flags, lstem, whole) if whole else (ti, flags, lstem), []).append(weld(Batch(
                ti, pos[used], uv[used], carry[used], li.reshape(-1, 3), flags, f"{part.name}:{dstem}")))
    return [(k, merge_batches(v)) for k, v in by.items()]


GLSHIM = Path.home() / "opt/glshim"   # libOpenGL.so.0 + libGLdispatch for pymeshlab's plugins (docs/world.md)
BOUNDARY_WEIGHT = float(__import__('os').environ.get('ASH_BW', 20.0))                 # meshlab quadric: penalty for moving open borders [придумал, по скринам]


def decimate(V: np.ndarray, T: np.ndarray, target: int, preserve_border: bool):
    """Quadric edge collapse. MeshLab (pymeshlab) when it loads: it can collapse open borders with a weighted
    penalty, so pieces made of many open fragments still reach the budget without shredding. Fallback pyfqmr."""
    T = T[(T[:, 0] != T[:, 1]) & (T[:, 1] != T[:, 2]) & (T[:, 0] != T[:, 2])]
    try:
        import pymeshlab
        ms = pymeshlab.MeshSet()
        ms.add_mesh(pymeshlab.Mesh(V, T.astype(np.int32)))
        ms.meshing_decimation_quadric_edge_collapse(targetfacenum=int(target), boundaryweight=BOUNDARY_WEIGHT,
                                                    preserveboundary=bool(preserve_border), planarquadric=True,
                                                    preservenormal=True, autoclean=True,
                                                    qualitythr=float(__import__('os').environ.get('ASH_QT', 0.6)),
                                                    preservetopology=bool(int(__import__('os').environ.get('ASH_PT', 0))))
        m = ms.current_mesh()
        return np.asarray(m.vertex_matrix(), np.float32), np.asarray(m.face_matrix(), np.int64)
    except Exception as ex:  # no pymeshlab / no libOpenGL
        if not getattr(decimate, "warned", False):
            print(f"  ! pymeshlab unavailable ({ex}), pyfqmr fallback", file=sys.stderr)
            decimate.warned = True
    import pyfqmr
    s = pyfqmr.Simplify()
    s.setMesh(V, T.astype(np.int32))
    s.simplify_mesh(target_count=int(target), aggressiveness=7, preserve_border=preserve_border, verbose=False)
    nv, nf, _ = s.getMesh()
    return np.asarray(nv, np.float32), np.asarray(nf, np.int64)


def is_whole(key) -> bool:
    """Batch key of a mesh that is not quadric-simplified: "whole" (keep_whole) or "blocks" (loose stone blocks)."""
    return isinstance(key, tuple) and len(key) > 3 and key[3] in ("whole", "blocks", "stairs")


def components(b: Batch) -> np.ndarray:
    """Loose piece label of every triangle (connected by shared vertex POSITIONS: the welded batch keeps separate
    vertices at UV seams, so by index a stone block fell apart into its faces and the blocks LOD (until 16.10) kept the
    biggest faces of the blocks - open boxes and lone pale faces (pt3 east wing) [проверено: m0150 m19_wall_13
    3426 кусков по индексам, 673 по позициям, медиана 10 треуг. = блок без дна])."""
    from scipy.sparse import coo_matrix
    from scipy.sparse.csgraph import connected_components
    _, inv = np.unique(np.round(b.pos / 1e-3).astype(np.int64), axis=0, return_inverse=True)
    t = inv.reshape(-1)[b.tri]
    n = int(t.max()) + 1 if t.size else 0
    g = coo_matrix((np.ones(t.size), (t.reshape(-1), np.roll(t, 1, axis=1).reshape(-1))), shape=(n, n))
    return connected_components(g, directed=False)[1][t[:, 0]]


def sub_batch(b: Batch, sel: np.ndarray) -> Batch:
    used, inv = np.unique(b.tri[sel], return_inverse=True)
    return Batch(b.tex, b.pos[used], b.uv[used], b.col[used], inv.reshape(-1, 3), b.flags, b.name)


_UNION_CACHE: dict = {}
_SPLIT_CACHE: dict = {}


def split_blocks(blk: list, max_tris=60, max_size=3.0):
    """Block meshes -> (blocks: loose pieces <= max_tris triangles and <= max_size m across, the rest as ordinary
    meshes keyed without "blocks"). Cached per batch: the cook asks many times."""
    key = tuple(id(b) for _, b in blk)
    hit = _SPLIT_CACHE.get(key)
    if hit is not None and all(a is b for a, (_, b) in zip(hit[0], blk)):
        return hit[1]
    small, big = [], []
    for k, b in blk:
        lab = components(b)
        cn = np.bincount(lab)
        P = b.pos[b.tri].reshape(-1, 3)
        lo = np.full((len(cn), 3), np.inf); hi = np.full((len(cn), 3), -np.inf)
        np.minimum.at(lo, np.repeat(lab, 3), P); np.maximum.at(hi, np.repeat(lab, 3), P)
        ok = (cn <= max_tris) & (np.linalg.norm(hi - lo, axis=1) <= max_size)
        sel = ok[lab]
        if sel.any():
            small.append((k, sub_batch(b, sel)))
        if (~sel).any():
            big.append((k[:3], sub_batch(b, ~sel)))
    res = (small, big)
    _SPLIT_CACHE[key] = ([b for _, b in blk], res)
    return res


def blocks_union(blk: list, cell=0.08, fat=0.03):
    """Loose stone blocks (M18["blocks"]) of one piece -> ONE closed surface (V, F) of their union, pre-decimated to the
    source block triangle count (cached per batch list: the cook asks for many targets). Each block is convex: voxels
    of `cell` m inside its hull (+`fat`, so touching blocks fuse) are filled, the boundary faces of the filled voxels are
    the surface; the quadric simplifier then reduces a solid instead of a heap of separate boxes.
    Why: simplified as separate boxes the blocks melted into floating slabs/diamonds; keeping the biggest blocks whole
    (until 16.10) left stacks with holes - pale cubes floating over the yard and the balcony ruins (pt3 route
    f1815/f4965). The union keeps the silhouette of the ruined pillars and merlons [урезание: камни сливаются в
    массив, грани скошены; проверено офлайн-рендером m0150 с камеры f4965]."""
    from scipy.spatial import ConvexHull
    key = tuple(id(b) for _, b in blk)
    hit = _UNION_CACHE.get(key)
    if hit is not None and all(a is b for a, (_, b) in zip(hit[0], blk)):
        return hit[1]
    pts = []
    for _, b in blk:
        lab = components(b)
        order = np.argsort(lab, kind="stable")
        bounds = np.searchsorted(lab[order], np.arange(lab.max() + 2))
        for c in range(lab.max() + 1):
            t = b.tri[order[bounds[c]:bounds[c + 1]]]
            pts.append(b.pos[np.unique(t)].astype(np.float64))
    lo = np.min([p.min(0) for p in pts], 0) - 2 * cell
    hi = np.max([p.max(0) for p in pts], 0) + 2 * cell
    dims = np.ceil((hi - lo) / cell).astype(int) + 1
    occ = np.zeros(dims, bool)
    for p in pts:
        try:
            h = ConvexHull(p)
        except Exception:   # flat or degenerate piece (decal): not a block
            continue
        a = np.floor((p.min(0) - fat - lo) / cell).astype(int)
        z = np.ceil((p.max(0) + fat - lo) / cell).astype(int) + 1
        g = np.stack(np.meshgrid(*[np.arange(a[i], z[i]) for i in range(3)], indexing="ij"), -1).reshape(-1, 3)
        inside = ((lo + (g + 0.5) * cell) @ h.equations[:, :3].T + h.equations[:, 3] <= fat).all(1)
        g = g[inside]
        occ[g[:, 0], g[:, 1], g[:, 2]] = True
    quads = []
    for ax in range(3):
        d = np.diff(occ.astype(np.int8), axis=ax)
        u, v = (ax + 1) % 3, (ax + 2) % 3
        eu = np.zeros(3, int); eu[u] = 1
        ev = np.zeros(3, int); ev[v] = 1
        for sgn in (1, -1):
            idx = np.argwhere(d == sgn)
            if len(idx):
                idx[:, ax] += 1
                c = [idx, idx + eu, idx + eu + ev, idx + ev]
                quads.append(np.stack(c if sgn == -1 else [c[0], c[3], c[2], c[1]], 1))
    if not quads:
        res = (np.zeros((0, 3)), np.zeros((0, 3), np.int64))
    else:
        q = np.concatenate(quads)
        flat = q.reshape(-1, 3)
        k = (flat[:, 0] * dims[1] + flat[:, 1]) * dims[2] + flat[:, 2]
        uk, inv = np.unique(k, return_inverse=True)
        inv = inv.reshape(-1, 4)
        gi = np.stack([uk // (dims[1] * dims[2]), (uk // dims[2]) % dims[1], uk % dims[2]], 1)
        V = lo + gi * cell
        F = np.concatenate([inv[:, [0, 1, 2]], inv[:, [0, 2, 3]]])
        res = decimate(V, F, sum(b.ntri for _, b in blk), False)
        res = (np.asarray(res[0], np.float64), np.asarray(res[1], np.int64))
    _UNION_CACHE[key] = ([b for _, b in blk], res)
    return res


def simplify_multi(items: list, target: int, preserve_border=False, drop_small=False, block_frac=1.0,
                   lod_far=False) -> list:
    """Quadric simplification of all batches of one piece together (texture seams are not borders, so the mesh can
    go much lower than per-texture simplification), then each new triangle takes texture, UV and lightmap UV from the
    closest original triangle (barycentric re-projection, as tools/ashm.simplify)."""
    if lod_far:   # stairs are whole only in the near LOD: from 8 m on a simplified flight is enough
        items = [((k[:3], b) if is_whole(k) and k[3] == "stairs" else (k, b)) for k, b in items]
    whole = [it for it in items if is_whole(it[0])]
    if whole:   # loose pieces: kept whole (keep_whole) or the blocks as one simplified solid (blocks_union)
        rest = [it for it in items if not is_whole(it[0])]
        blk = [(k, b) for k, b in whole if k[3] == "blocks"]
        out = [(k, b) for k, b in whole if k[3] != "blocks" and b.ntri]
        if blk and block_frac < 1.0:
            # parts of a block mesh that are not loose blocks (whole wall sections) simplify with the rest
            blk, big = split_blocks(blk)
            rest = rest + big
        nr = sum(b.ntri for _, b in rest)
        t = int(target * nr / max(1, nr + sum(b.ntri for _, b in whole)))   # the rest keeps its share
        if blk and block_frac >= 1.0:
            out += blk
        elif blk:
            nb = sum(b.ntri for _, b in blk)
            V, F = blocks_union(blk) if blk else (np.zeros((0, 3)), np.zeros((0, 3), np.int64))
            if len(F):
                nv, nf = decimate(V, F, max(12, int(nb * block_frac)), False)
                fn = np.cross(nv[nf[:, 1]] - nv[nf[:, 0]], nv[nf[:, 2]] - nv[nf[:, 0]])
                nf = nf[fn[:, 1] / (np.linalg.norm(fn, axis=1) + 1e-12) > -0.7]   # bottoms rest on walls: unseen
                nf = nf[:, ::-1]   # the union faces out counter-clockwise; FromSoft front faces are clockwise
                if len(nf):
                    out += reproject(blk, nv, nf, max(12, int(nb * block_frac)), keep_winding=True, box_uv=True)
        return out + (simplify_multi(rest, max(8, t), preserve_border, drop_small) if rest else [])
    total = sum(b.ntri for _, b in items)
    if total <= target or target < 4:
        return items
    P = np.concatenate([b.pos for _, b in items])
    T, off = [], 0
    for _, b in items:
        T.append(b.tri + off); off += len(b.pos)
    T = np.concatenate(T)
    _, pfirst, pinv = np.unique(np.round(P / 1e-3).astype(np.int64), axis=0, return_index=True, return_inverse=True)
    pinv = pinv.reshape(-1)
    nv, nf = decimate(P[pfirst].astype(np.float64), pinv[T], int(target), preserve_border)
    if len(nf) == 0:
        return items
    return reproject(items, nv, nf, target, drop_small)


def reproject(items: list, nv: np.ndarray, nf: np.ndarray, target: int, drop_small=False, keep_winding=False,
              box_uv=False) -> list:
    """Simplified triangles (nv, nf) -> [(key, Batch)] of `items`: each new triangle takes texture, UV and lightmap UV
    from the closest original triangle of similar orientation (barycentric re-projection) and its winding
    (keep_winding: a closed surface already facing out, blocks_union - the closest original may be a side face).
    box_uv (blocks_union): a fused face spans many blocks, extrapolated UVs smeared into stripes - instead the diffuse
    UV is a box projection at the texel density of the original blocks; the lightmap UV stays the clamped barycentric
    one of the closest same-facing original triangle (the nearest original VERTEX was often on a hidden face between
    two blocks, black in the lightmap: dark streaks) [придумал]."""
    from scipy.spatial import cKDTree
    P = np.concatenate([b.pos for _, b in items]); UV = np.concatenate([b.uv for _, b in items])
    C = np.concatenate([b.col for _, b in items])
    T, M, off = [], [], 0
    for i, (_, b) in enumerate(items):
        T.append(b.tri + off); M.append(np.full(b.ntri, i)); off += len(b.pos)
    T = np.concatenate(T); M = np.concatenate(M)
    a, bb, c = P[T[:, 0]], P[T[:, 1]], P[T[:, 2]]
    oc = (a + bb + c) / 3
    on = np.cross(bb - a, c - a); on /= np.linalg.norm(on, axis=1, keepdims=True) + 1e-12
    nc = nv[nf].mean(1)
    nn = np.cross(nv[nf[:, 1]] - nv[nf[:, 0]], nv[nf[:, 2]] - nv[nf[:, 0]])
    nn /= np.linalg.norm(nn, axis=1, keepdims=True) + 1e-12
    k = min(12, len(T))
    _, cand = cKDTree(oc).query(nc, k=k)
    cand = cand.reshape(len(nf), k)
    d = np.linalg.norm(oc[cand] - nc[:, None, :], axis=2)
    nd = 1.0 - np.einsum("ij,ikj->ik", nn, on[cand])
    best = cand[np.arange(len(nf)), np.argmin(d + nd * 0.5, axis=1)]
    corners = nv[nf]
    A, B, Cc = a[best], bb[best], c[best]
    v0, v1 = B - A, Cc - A
    d00 = (v0 * v0).sum(1); d01 = (v0 * v1).sum(1); d11 = (v1 * v1).sum(1)
    den = d00 * d11 - d01 * d01
    den = np.where(np.abs(den) < 1e-12, 1e-12, den)
    v2 = corners - A[:, None, :]
    d20 = (v2 * v0[:, None, :]).sum(2); d21 = (v2 * v1[:, None, :]).sum(2)
    wv = (d11[:, None] * d20 - d01[:, None] * d21) / den[:, None]
    ww = (d00[:, None] * d21 - d01[:, None] * d20) / den[:, None]
    wu = 1 - wv - ww
    to = T[best]
    uv = wu[..., None] * UV[to[:, 0]][:, None] + wv[..., None] * UV[to[:, 1]][:, None] + ww[..., None] * UV[to[:, 2]][:, None]
    cw = np.clip(np.stack([wu, wv, ww], -1), 0, None); cw /= cw.sum(-1, keepdims=True) + 1e-9
    col = cw[..., 0:1] * C[to[:, 0]][:, None] + cw[..., 1:2] * C[to[:, 1]][:, None] + cw[..., 2:3] * C[to[:, 2]][:, None]
    if box_uv:
        dens = []
        for _, b in items:   # UV units per metre of the original blocks (median over edges)
            e = b.tri[:, [0, 1]]
            dp = np.linalg.norm(b.pos[e[:, 0]] - b.pos[e[:, 1]], axis=1)
            du = np.linalg.norm(b.uv[e[:, 0]] - b.uv[e[:, 1]], axis=1)
            ok = dp > 0.05
            dens.append(float(np.median(du[ok] / dp[ok])) if ok.any() else 1.0)
        ax = np.abs(nn).argmax(1)
        ua, va = (ax + 1) % 3, (ax + 2) % 3
        r = np.arange(len(nf))
        k = np.array(dens)[M[best]][:, None]
        uv = np.stack([corners[r[:, None], np.arange(3)[None, :], ua[:, None]],
                       corners[r[:, None], np.arange(3)[None, :], va[:, None]]], -1) * k[..., None]
        uv -= np.floor(uv.min(1, keepdims=True))   # keep the numbers small (the GE repeats the texture)
    flip = np.einsum("ij,ij->i", nn, on[best]) < 0
    if keep_winding:
        flip[:] = False
    corners[flip] = corners[flip][:, ::-1]; uv[flip] = uv[flip][:, ::-1]; col[flip] = col[flip][:, ::-1]
    mat = M[best]
    if drop_small and len(nf) > target * 1.5:
        # disconnected cards (foliage, bars) do not collapse: keep the biggest triangles  [придумал]
        area = np.linalg.norm(np.cross(corners[:, 1] - corners[:, 0], corners[:, 2] - corners[:, 0]), axis=1)
        keep = np.sort(np.argsort(-area)[: int(target)])
        corners, uv, col, mat = corners[keep], uv[keep], col[keep], mat[keep]
    out = []
    for i, (key, b) in enumerate(items):
        sel = mat == i
        n = int(sel.sum())
        if n == 0:
            continue
        nb = Batch(b.tex, corners[sel].reshape(-1, 3).astype(np.float32), uv[sel].reshape(-1, 2).astype(np.float32),
                   col[sel].reshape(-1, 3).astype(np.float32), np.arange(n * 3).reshape(n, 3), b.flags, b.name)
        out.append((key, weld(nb)))
    return out


def shape_pair(orig: list, simp: list):
    """(orig, simp) for geom_error. The fused blocks are compared with their full union surface (blocks_union before
    decimation), not with the separate blocks: the union has no faces where blocks touch, and those hidden faces
    counted as "holes" boosted the pieces with ruins x2.5-5 (m0045, m0046) for nothing [проверено: кук 16.10,
    boost_near до/после]. Leaving the blocks out of the measure is worse: their area diluted the error of the rest
    before, without them the boosts of the other pieces doubled and the global ratio fell 0.056 -> 0.038."""
    blk = [(k, b) for k, b in orig if is_whole(k) and k[3] == "blocks"]
    if not blk:
        return orig, simp
    small, big = split_blocks(blk)
    o = [(k, b) for k, b in orig if not (is_whole(k) and k[3] == "blocks")] + big
    if small:
        V, F = blocks_union(small)
        if len(F):
            o.append((("union",), Batch(0, V.astype(np.float32), np.zeros((len(V), 2), np.float32),
                                         np.zeros((len(V), 3), np.float32), F, 0, "union")))
    return o, simp


def geom_error(orig: list, simp: list, walk_tree, near=40.0, density=12.0, k_tol=0.065, seed=1):
    """Shape error of a simplification as the player sees it: (shard, hole) = area fraction of the simplified surface
    with no original surface nearby in the same orientation (spikes, slabs across gaps, wedges), and of the original
    surface with no simplified surface nearby (missing walls, columns). "Nearby" = within max(0.3 m, k_tol * distance
    to the closest camera spot) with |normal . normal| > 0.8, camera spots = walkable floor + 1.6 m, so a 0.3 m error
    counts at 4.6 m and a 1.3 m error at 20 m (screen-space-ish). Only surface within `near` m of the floor.
    Area-uniform point samples; noise floor ~0.4% [проверено на m18_01]. None when nothing is near the floor."""
    from scipy.spatial import cKDTree
    rng = np.random.default_rng(seed)

    def pts(items):
        out, wts, nrm = [], [], []
        for _, b in items:
            if b.ntri == 0:
                continue
            T = b.pos[b.tri].astype(np.float64)
            n = np.cross(T[:, 1] - T[:, 0], T[:, 2] - T[:, 0])
            a2 = np.linalg.norm(n, axis=1)
            k = np.clip(np.round(0.5 * a2 * density).astype(int), 1, 4000)
            i = np.repeat(np.arange(len(T)), k)
            r1 = np.sqrt(rng.random(len(i))); r2 = rng.random(len(i))
            w = np.stack([1 - r1, r1 * (1 - r2), r1 * r2], 1)
            out.append((T[i] * w[..., None]).sum(1)); wts.append(0.5 * a2[i] / k[i])
            nrm.append(n[i] / (a2[i, None] + 1e-12))
        if not out:
            return (np.zeros((0, 3)),) * 4
        p, w, n = np.concatenate(out), np.concatenate(wts), np.concatenate(nrm)
        dw = walk_tree.query(p, distance_upper_bound=near)[0]
        m = dw < near
        return p[m], w[m], n[m], dw[m]

    def bad(pa, wa, na, da, pb, nb):
        tol = np.maximum(0.3, k_tol * da)
        dd, jj = cKDTree(pb).query(pa, k=12, distance_upper_bound=float(tol.max()))
        ok = np.zeros(len(pa), bool)
        for c in range(dd.shape[1]):
            v = dd[:, c] < tol
            j = np.where(v, jj[:, c], 0)
            ok |= v & (np.abs((na * nb[j]).sum(1)) > 0.8)
        return float((wa * ~ok).sum() / wa.sum())
    po, wo, no, do = pts(orig)
    if len(po) == 0:
        return None
    ps, ws, ns, ds = pts(simp)
    if len(ps) == 0:
        return 0.0, 1.0
    return bad(ps, ws, ns, ds, po, no), bad(po, wo, no, do, ps, ns)


def bake_light(b: Batch, lstem, lm_cache, cfg) -> Batch:
    if lstem:
        col = bilinear(lm_cache[lstem], b.col[:, :2]) * cfg["lm_scale"] + cfg["lm_ambient"]
        b.col = (col / np.maximum(1.0, col.max(1, keepdims=True))).astype(np.float32)
    else:
        b.col = np.full_like(b.col, 0.35)
    return b


def cook_map(cfg, msb, bank: TexBank, stats: dict, edge_limit, budget_scale=1.0):
    lm_cache: dict = {}
    flv_cache: dict = {}
    walk_tree = getattr(edge_limit, "tree", None)   # walkable floor points (FS space), see EdgeLimit
    pieces = []   # (part, mask, [(key, batch)])
    src_total = 0
    placed = {}
    for part in msb.map_pieces:
        model = part.model.name
        if model in cfg["cut"]:
            continue
        # the same model placed twice at the same spot for different draw groups (m5101B1_0000/_0001,
        # m9100B1/_0000/_0001): cook once, union of the masks
        where = (model, tuple(np.round([float(v) for v in part.translate], 2)),
                 tuple(np.round([float(v) for v in part.rotate], 1)))
        mk = mask128(bits(part.draw_groups))
        if where in placed:
            i = placed[where]
            p0 = pieces[i]
            pieces[i] = (p0[0], p0[1], tuple(a | b for a, b in zip(p0[2], mk)), p0[3], p0[4])
            continue
        placed[where] = len(pieces)
        if model not in flv_cache:
            flv_cache[model] = FLVER.from_path(DSR / "map" / cfg["map"] / f"{model}A{cfg['area'][1:]}.flver.dcx")
        skip = SKIP_MTD if model not in cfg["sky"] else ("Water", "Fly", "insect", "Snow", "light_shaft")
        bs = load_piece_batches(cfg, part, flv_cache[model], bank, lm_cache, skip)
        n = sum(b.ntri for _, b in bs)
        if model not in cfg["sky"]:
            src_total += n * cfg["weights"].get(model, 1.0)
        pieces.append((part, model, mk, bs, n))

    ratio = cfg["world_budget"] * budget_scale / max(1, src_total)
    # Per-piece quality boosts: a piece whose simplification tears where the player walks (spikes, slabs, holes, see
    # geom_error) gets more triangles, the global ratio below takes them from the pieces that simplify cleanly.
    # The far LOD (drawn from lod_dist m) gets its own, looser bar: at 0.15 of the near LOD it was 37% spikes on the
    # cliff path m5000 (docs/world.md). Boosts are measured once at the start ratio.
    boost = {}
    qn, qf = cfg["quality_near"], cfg["quality_far"]
    for part, model, mask, bs, n in pieces:
        if model in cfg["sky"] or walk_tree is None:
            continue
        r0 = min(1.0, ratio * cfg["weights"].get(model, 1.0))
        t = max(8, int(n * r0))
        e = None
        for _ in range(6):
            e = geom_error(*shape_pair(bs, simplify_multi(bs, t, cfg["preserve_border"],
                                                          block_frac=min(1.0, t / max(1, n) * cfg.get("blocks_w", 1.0)))),
                           walk_tree)
            if e is None or (e[0] <= qn[0] and e[1] <= qn[1]) or t >= n:
                break
            t = min(n, int(t * 1.6))
        tf = max(8, int(t * cfg.get("far_frac", {}).get(model, cfg["lod_far_frac"])))
        ef = None
        for _ in range(6):
            ef = geom_error(*shape_pair(bs, simplify_multi(bs, tf, cfg["preserve_border"], lod_far=True,
                                               block_frac=min(1.0, tf / max(1, n) * cfg.get("blocks_w", 1.0)))), walk_tree)
            if ef is None or (ef[0] <= qf[0] and ef[1] <= qf[1]) or tf >= t:
                break
            tf = min(t, int(tf * 1.5))
        boost[part.name] = (t / max(1.0, n * r0), tf / max(1.0, n * r0), e, ef)

    def run(ratio):
        out, per_piece = [], {}
        for part, model, mask, bs, n in pieces:
            bn = bf = None
            if model in cfg["sky"]:
                budget, edge, flags = cfg["sky"][model]
                r = min(1.0, budget / max(1, n))
            else:
                r, edge, flags = min(1.0, ratio * cfg["weights"].get(model, 1.0)), cfg["max_edge"], 0
                bn, bf = boost.get(part.name, (1.0, cfg.get("far_frac", {}).get(model, cfg["lod_far_frac"]), None, None))[:2]
                rf = min(1.0, r * bf)
                r = min(1.0, r * bn)
            cnt = lo_cnt = 0
            simp = simplify_multi(bs, max(8, int(n * r)), cfg["preserve_border"], block_frac=min(1.0, r * cfg.get("blocks_w", 1.0)))
            pre = sum(b.ntri for _, b in simp)
            for key, s in simp:
                tsl = tessellate(s, edge) if model in cfg["sky"] else tessellate_adaptive(s, edge_limit)
                tsl = bake_light(tsl, key[2], lm_cache, cfg)
                tsl.flags |= flags if model in cfg["sky"] else BF_LOD_NEAR
                out.append((part, model, mask, tsl))
                cnt += tsl.ntri
            if model not in cfg["sky"]:
                # far LOD: drawn only for chunks farther than the LOD distance (16 m; chunk 12 m -> geometry >= ~6 m away)
                # so a coarse 6 m tessellation is enough (see EdgeLimit)
                for key, s in simplify_multi(bs, max(8, int(n * rf)), cfg["preserve_border"], lod_far=True,
                                             block_frac=min(1.0, rf * cfg.get("blocks_w", 1.0))):
                    t2 = bake_light(tessellate(s, cfg["lod_far_edge"]), key[2], lm_cache, cfg)
                    t2.flags |= BF_LOD_FAR
                    out.append((part, model, mask, t2))
                    lo_cnt += t2.ntri
            per_piece[part.name] = dict(model=model, src=n, simplified=pre, cooked=cnt, far_lod=lo_cnt)
            if part.name in boost:
                b0 = boost[part.name]
                per_piece[part.name].update(boost_near=round(b0[0], 2), boost_far=round(b0[1], 3),
                                            err_near=b0[2] and [round(v, 3) for v in b0[2]],
                                            err_far=b0[3] and [round(v, 3) for v in b0[3]])
        return out, per_piece

    out, per_piece = run(ratio)
    got = sum(b.ntri for _, m, _, b in out if b.flags & BF_LOD_NEAR)
    target = cfg["world_budget"] * budget_scale
    for _ in range(4):   # tessellation is not proportional to the simplification ratio: iterate
        if abs(got - target) <= target * 0.05:
            break
        ratio *= (target / got) ** 1.3
        out, per_piece = run(ratio)
        got = sum(b.ntri for _, m, _, b in out if b.flags & BF_LOD_NEAR)
        print(f"  ratio {ratio:.4f} -> world {got} tris", file=sys.stderr)
    stats["map"] = dict(src_tris=int(sum(p[4] for p in pieces)), ratio=round(ratio, 4), world_tris=int(got),
                        far_lod_tris=int(sum(b.ntri for _, m, _, b in out if b.flags & BF_LOD_FAR)),
                        sky_tris=int(sum(b.ntri for _, m, _, b in out if m in cfg["sky"])), pieces=per_piece)
    return out


# --------------------------------------------------------------------------- objects

def obj_binder(model):
    return Binder.from_path(DSR / "obj" / f"{model}.objbnd.dcx")


def cook_obj_model(model, budget, bank: TexBank):
    """Object FLVER -> list of Batch in FS object-local space (col = normals for later shading)."""
    b = obj_binder(model)
    try:
        f = binder_flver(b)
    except FileNotFoundError:
        return [], 0
    tx = binder_textures(b)
    tree = f.get_bone_tree().get_bone_armature_space_transforms()
    parts, src = [], 0
    for m in valid_meshes(f):
        if m.is_dynamic and len(f.bones) > 8:
            continue   # skinned (corpses, rats): not cooked
        texs = mesh_textures(m)
        ds = texs.get("g_Diffuse")
        if not ds:
            continue
        d = mesh_arrays(m)
        tri = mesh_triangles(m)
        if len(tri) == 0 or "uv0" not in d:
            continue
        pos = d["pos"]; nrm = d.get("normal", np.tile([0, 1, 0], (len(pos), 1)).astype(np.float32))[:, :3]
        bi = m.default_bone_index if m.default_bone_index >= 0 else 0
        if not m.is_dynamic and bi < len(tree):
            tb, rb = tree[bi][0], tree[bi][1]
            Rb = np.array(rb.data, np.float32); Tb = np.array(list(tb), np.float32)
            pos = pos @ Rb.T + Tb; nrm = nrm @ Rb.T
        src += len(tri)
        if model in FLOOR_PLATES:
            tri = plate_mesh_tris(pos, nrm, tri)
        for ds, uv, ltri in diffuse_layers(m, d, texs, tri):
            if ds in tx:
                ti = bank.add(("obj", ds), lambda ds=ds: tx[ds][0])
            else:
                ti = bank.add(("map", ds), map_tex_loader(ds))
            if ti is None:
                continue
            flags = BF_ALPHA | BF_NOCULL if bank.items[ti].alpha else 0
            used, li = np.unique(ltri, return_inverse=True)
            parts.append(weld(Batch(ti, pos[used].astype(np.float32), uv[used], nrm[used].astype(np.float32),
                                    li.reshape(-1, 3), flags, f"{model}:{ds}")))
    if model in FLOOR_PLATES:
        return [tessellate(p, 3.0) for p in parts], src   # loose slabs: the simplifier turns them into shards
    simp = simplify_multi([(i, p) for i, p in enumerate(parts)], max(6, budget), preserve_border=False, drop_small=True)
    return [tessellate(p, 3.0) for _, p in simp], src


def plate_mesh_tris(pos, nrm, tri, up=0.9, slab_share=0.4):
    """Triangles of a FLOOR_PLATES mesh kept instead of simplification. o8510 (the hall floor over the pit) is ~150
    separate flagstones (1690 tris, 34 loose pieces) + rubble chunks: quadric simplification to the object budget
    collapsed them into tilted slabs with holes ("floating tiles" under the Demon, pt2-death f2826 [проверено:
    offline render src vs cooked]). A slab mesh (upward faces >= slab_share of its area) keeps only its upward faces
    - the flagstones' sides are 0.1-0.4 m bevels, unseen from the player camera [урезание]; other meshes (rubble
    chunks) stay whole. o8510: 2225 -> 863 tris, no simplification."""
    P = pos[tri]
    a = np.linalg.norm(np.cross(P[:, 1] - P[:, 0], P[:, 2] - P[:, 0]), axis=1)
    ny = nrm[tri][:, :, 1].mean(1) / (np.linalg.norm(nrm[tri].mean(1), axis=1) + 1e-9)
    top = ny > up
    if a[top].sum() >= slab_share * a.sum():
        return tri[top]
    return tri


LDIR = np.array([0.35, 0.8, -0.45]); LDIR /= np.linalg.norm(LDIR)


def shade(nrm: np.ndarray, base) -> np.ndarray:
    n = nrm / (np.linalg.norm(nrm, axis=1, keepdims=True) + 1e-9)
    lam = np.clip(n @ LDIR, 0, 1)[:, None]
    return np.clip(np.asarray(base, np.float32)[None, :] * (0.55 + 0.6 * lam), 0, 1).astype(np.float32)


def obj_boxes(model, mesh_batches) -> list:
    """HKX rigid-body boxes of the object (FS local): [(center, half, flags)]. Fallback: AABB of the render mesh."""
    from soulstruct.havok.core import HKX
    out = []
    try:
        b = obj_binder(model)
    except Exception:
        return out
    for e in b.entries:
        nm = e.name.replace("\\", "/").rsplit("/", 1)[-1]
        if nm.lower() != f"{model}.hkx":
            continue
        try:
            ps = HKX.from_bytes(e.data).root.namedVariants[0].variant
            for sy in ps.systems:
                for rb in sy.rigidBodies:
                    sh = rb.collidable.shape
                    xf = np.array(list(rb.motion.motionState.transform), np.float64)
                    t = xf[12:15] if len(xf) == 16 else np.zeros(3)
                    off = np.zeros(3)
                    if type(sh).__name__ == "hkpConvexTranslateShape":
                        off = np.array(list(sh.translation)[:3]); sh = sh.childShape
                        sh = getattr(sh, "childShape", sh)   # hkpSingleShapeContainer
                    if type(sh).__name__ == "hkpBoxShape":
                        h = np.array(list(sh.halfExtents)[:3]) + float(sh.radius)
                        out.append(((t + off).tolist(), h.tolist(), 0))
        except Exception as ex:  # pragma: no cover
            print(f"  ! hkx {model}: {ex}")
    if not out and mesh_batches:
        P = np.concatenate([b.pos for b in mesh_batches])
        lo, hi = P.min(0), P.max(0)
        out.append((((lo + hi) / 2).tolist(), ((hi - lo) / 2).tolist(), 1))   # flag 1 = from render mesh [придумал]
    return out


def fog_quad(bank: TexBank, half, center) -> list[Batch]:
    """Fog wall: one double-sided additive quad in the box's XY plane (FS local) + a soft white noise texture."""
    from PIL import Image
    rng = np.random.default_rng(1810)
    n = rng.random((64, 32))
    for _ in range(3):
        n = (n + np.roll(n, 1, 0) + np.roll(n, 1, 1) + np.roll(n, -1, 0)) / 4
    n = (n - n.min()) / (n.max() - n.min())
    g = (90 + 140 * n).astype(np.uint8)
    im = Image.fromarray(np.dstack([g, g, np.clip(g.astype(int) + 20, 0, 255).astype(np.uint8),
                                    np.full_like(g, 255)]), "RGBA")
    ti = bank.add(("gen", "fogwall"), lambda: im)
    cx, cy, cz = center; hx, hy, _ = half
    P = np.array([[cx - hx, cy - hy, cz], [cx + hx, cy - hy, cz], [cx + hx, cy + hy, cz], [cx - hx, cy + hy, cz]],
                 np.float32)
    UV = np.array([[0, 2], [1, 2], [1, 0], [0, 0]], np.float32)
    b = Batch(ti, P, UV, np.full((4, 3), 0.55, np.float32), np.array([[0, 1, 2], [0, 2, 3]]),
              BF_NOCULL | BF_ADD, "fogwall")
    return [tessellate(b, 1.5)]


# --------------------------------------------------------------------------- main

FLOOR_PLATES = ("o8510",)   # objects that ARE the floor: the hall floor over the pit has a hole in the map HKX


def floor_plate_parts(map_id):
    """Extra collision parts for FLOOR_PLATES (their HKX is a MOPP tree we do not decode): a flat quad at the top
    of the object's render mesh AABB, placed by the MSB (FS space, cook_collision part dict). [придумал: plate]"""
    from soulstruct.darksouls1r.maps import MSB
    msb = MSB.from_path(DSR / "map" / "MapStudio" / f"{map_id}.msb")
    out = []
    for o in msb.objects:
        if o.model.name not in FLOOR_PLATES:
            continue
        f = binder_flver(obj_binder(o.model.name))
        P = np.concatenate([mesh_arrays(m)["pos"] for m in valid_meshes(f)])
        lo, hi = P.min(0), P.max(0)
        # walking height = the area-dominant level of the upward faces (rubble on top would lift the AABB top)
        tris = np.concatenate([mesh_arrays(m)["pos"][mesh_triangles(m)] for m in valid_meshes(f)])
        n = np.cross(tris[:, 1] - tris[:, 0], tris[:, 2] - tris[:, 0])
        area = np.linalg.norm(n, axis=1)
        up = np.abs(n[:, 1]) > 0.9 * area
        hist = {}
        for y, a in zip(np.round(tris[up, :, 1].mean(1), 2), area[up]):
            hist[y] = hist.get(y, 0) + a
        top = max(hist, key=hist.get) if hist else hi[1]
        q = np.array([[lo[0], top, lo[2]], [hi[0], top, lo[2]], [hi[0], top, hi[2]], [lo[0], top, hi[2]]])
        R = euler_fs(o.rotate)
        v = q @ R.T + np.array([float(x) for x in o.translate])
        # FromSoft front faces are CW seen from the front: (0, 2, 1) / (0, 3, 2) face up after the PSP mirror
        out.append(dict(name=o.name, model=o.model.name, cam_row=-1,
                        meshes=[(v, np.array([[0, 1, 2], [0, 2, 3]]), 1)]))
    return out


def cook_pcol(cfg):
    """Collision of the playable box through tools/cook_collision.py (format v1, owned by that cooker)."""
    import contextlib
    import io
    import tempfile
    import cook_collision as cc
    lo, hi = cfg["col_box"]
    cc.SCENES["zone"] = dict(map=cfg["map"], box_min=lo, box_max=hi, cell=cfg["col_cell"], skip=cfg["col_skip"],
                             target="c2232_0000", player=(-15.45, 184.7, -46.8, -125.0))  # START region 1812011
    orig_load = cc.load_parts

    def load_parts_plus(map_id):
        parts = orig_load(map_id)
        parts += floor_plate_parts(map_id)
        return parts
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
            cc.load_parts = orig_load
        return p.read_bytes(), json.loads(p.with_suffix(".json").read_text())



def in_box(p, box):
    lo, hi = box
    return all(lo[i] <= p[i] <= hi[i] for i in range(3))


def main():
    import os
    if GLSHIM.is_dir() and str(GLSHIM) not in os.environ.get("LD_LIBRARY_PATH", ""):
        env = dict(os.environ, LD_LIBRARY_PATH=f"{GLSHIM}:{os.environ.get('LD_LIBRARY_PATH', '')}")
        os.execve(sys.executable, [sys.executable] + sys.argv, env)
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True, help="zone_<map>.pak")
    ap.add_argument("--budget", type=int, default=None, help="world triangle budget (map pieces, default 46000)")
    ap.add_argument("--clut", type=int, default=4, choices=(4, 8))
    ap.add_argument("--tex", type=int, default=128, help="max texture size")
    ap.add_argument("--keep-underground", action="store_true", help="keep the second demon's pit (not in S1)")
    ap.add_argument("--max-pak-mb", type=float, default=9.0)
    args = ap.parse_args()
    t0 = time.time()
    cfg = dict(M18)
    if args.budget:
        cfg["world_budget"] = args.budget
    if args.keep_underground:
        cfg["cut"] = tuple(c for c in cfg["cut"] if c not in ("m0301B1", "m0310B1", "m0320B1"))
        cfg["col_skip"] = ()
    from soulstruct.darksouls1r.maps import MSB
    msb = MSB.from_path(DSR / "map" / "MapStudio" / f"{cfg['map']}.msb")
    bank = TexBank(args.tex, args.clut)
    stats: dict = {"map": cfg["map"]}

    # ---- map pieces -> chunked world batches (PSP space)
    pcol, cstats = cook_pcol(cfg)
    stats["collision"] = {k: v for k, v in cstats.items() if k != "parts"}
    stats["collision"]["parts"] = len(cstats["parts"])
    edge_limit = EdgeLimit(walkable_points(pcol))
    cooked = cook_map(cfg, msb, bank, stats, edge_limit)
    groups: dict = {}
    gmask: dict = {}
    sky = []
    for part, model, mask, b in cooked:
        b.pos = b.pos * MIRROR
        if b.flags & (BF_SKY | BF_FAR):
            for s in split_chunks(b, 1e6 if b.flags & BF_SKY else 60.0):
                for u in split_u16(s):
                    sky.append(WBatch(u.tex, u.pos, u.uv, u.col, u.tri, u.flags, (0xFFFFFFFF,) * 4, u.name))
            continue
        for s in split_chunks(b, cfg["chunk"]):
            # one batch per (texture, flags, chunk): pieces of the same chunk merge, their draw-group masks OR-ed
            # (slightly more conservative visibility, ~2x fewer draw calls)
            key = (s.tex, s.flags, tuple(np.floor(s.pos[s.tri].mean((0, 1)) / cfg["chunk"]).astype(int)))
            groups.setdefault(key, []).append(s)
            gmask[key] = tuple(a | b for a, b in zip(gmask.get(key, (0, 0, 0, 0)), mask))
    world = []
    for (tex, flags, ck), bs in groups.items():
        mask = gmask[(tex, flags, ck)]
        m = merge_batches(bs) if len(bs) > 1 else bs[0]
        code = (ck[0] + 512) | ((ck[1] + 512) << 10) | ((ck[2] + 512) << 20)
        for u in split_u16(m):
            world.append(WBatch(u.tex, u.pos, u.uv, u.col, u.tri, flags, mask, u.name, code))

    # KD-tree of baked map light for object shading
    from scipy.spatial import cKDTree
    allp = np.concatenate([b.pos for b in world]); allc = np.concatenate([b.col for b in world])
    light_tree = cKDTree(allp)

    def light_at(p_psp):
        d, i = light_tree.query(p_psp, k=24)
        w = 1.0 / (d + 0.5)
        c = (allc[i] * w[:, None]).sum(0) / w.sum()
        return np.clip(c * 1.15, 0.08, 1.0)

    # ---- collisions (display / draw groups) and objects
    cols_by_name = {c.name: c for c in msb.collisions}
    zone = Zone()
    for c in msb.collisions:
        zone.cols.append(dict(name=c.name, disp=mask128(bits(c.display_groups)), draw=mask128(bits(c.draw_groups)),
                              cam_row=int(c.camera_1_id), sound=int(c.sound_space_type),
                              reflect=float(c.reflect_plane_height)))

    def part_mask(p):
        b = bits(p.draw_groups)
        if not b and getattr(p, "draw_parent", None) is not None and p.draw_parent.name in cols_by_name:
            b = bits(cols_by_name[p.draw_parent.name].draw_groups)
        return mask128(b) if b else (0xFFFFFFFF,) * 4

    def cut_pit(p):
        return not args.keep_underground and in_box(p, cfg["pit_box"])

    model_cache: dict = {}
    meshes, mesh_batches = [], []
    mesh_index: dict = {}
    decor_tris = 0
    obj_stats = {}
    for o in msb.objects:
        model = o.model.name
        pos_fs = np.array([float(v) for v in o.translate])
        if model in OBJ_SKIP or cut_pit(pos_fs):
            continue
        R_fs = euler_fs(o.rotate)
        kind, budget = OBJ_KIND.get(model, ("DECOR", DECOR_BUDGET.get(model, 80)))
        if model not in model_cache:
            mb, src = cook_obj_model(model, budget, bank) if budget > 0 else ([], 0)
            boxes = obj_boxes(model, mb)
            model_cache[model] = (mb, boxes, src)
            obj_stats[model] = dict(kind=kind, src=src, tris=sum(b.ntri for b in mb), boxes=len(boxes))
        mb, boxes, _ = model_cache[model]
        pos_psp = pos_fs * MIRROR
        R_psp = to_psp_rot(R_fs)
        mask = part_mask(o)
        if kind == "DECOR" or model == "o8550":
            base = light_at(pos_psp + [0, 0.8, 0])
            for b in mb:
                P = (b.pos @ R_fs.T + pos_fs) * MIRROR
                N = (b.col @ R_fs.T) * MIRROR
                world.append(WBatch(b.tex, P.astype(np.float32), b.uv, shade(N, base), b.tri,
                                    b.flags | (0 if model == "o8550" else BF_DECOR), mask, f"{o.name}"))
                decor_tris += b.ntri
            if model == "o8550":
                zone.objs.append(dict(name=o.name, entity=int(o.entity_id), kind=KINDS["NEST"], mesh=-1, boxes=[],
                                      flags=0, rot=R_psp, pos=pos_psp, mask=mask))
            continue
        if model not in mesh_index:
            if kind == "FOG":
                c, h, _ = boxes[0] if boxes else ([0, 2.9, 0], [1.7, 2.9, 0.2], 0)
                mb = fog_quad(bank, h, c)
            start = len(mesh_batches)
            rad = 0.0
            for b in mb:
                P = (b.pos * MIRROR).astype(np.float32)
                col = shade(b.col * MIRROR, (0.62, 0.6, 0.56)) if not b.flags & BF_ADD else b.col
                mesh_batches.append(WBatch(b.tex, P, b.uv, col, b.tri, b.flags | BF_MESH, (0, 0, 0, 0), b.name))
                rad = max(rad, float(np.linalg.norm(P, axis=1).max()))
            mesh_index[model] = len(meshes)
            meshes.append((start, len(mesh_batches) - start, rad))
        zone.objs.append(dict(name=o.name, entity=int(o.entity_id), kind=KINDS[kind], mesh=mesh_index[model],
                              boxes=[((np.array(c) * MIRROR).tolist(), h, f) for c, h, f in boxes], flags=0,
                              rot=R_psp, pos=pos_psp, mask=mask))
    stats["objects"] = dict(models=obj_stats, decor_tris=decor_tris, dynamic=len(zone.objs),
                            mesh_tris=sum(b.tri.shape[0] for b in mesh_batches))

    # ---- characters
    core_hollows = 0
    for c in msb.characters:
        pos_fs = np.array([float(v) for v in c.translate])
        if cut_pit(pos_fs):
            continue
        flags = 0
        if int(c.entity_id) == 1810800:
            flags |= CHR_BOSS | CHR_CORE
        if c.model.name == "c2500" and core_hollows < 8:
            flags |= CHR_CORE
            core_hollows += 1
        pr = [r for r in getattr(c, "patrol_regions", []) if r is not None][:2]
        zone.chrs.append(dict(model=c.model.name, entity=int(c.entity_id), pos=(pos_fs * MIRROR).tolist(),
                              yaw=-math.radians(float(c.rotate[1])), npc=int(c.character_id),
                              think=int(c.ai_id), talk=int(c.talk_id), patrol_names=[r.name for r in pr],
                              flags=flags, mask=part_mask(c)))

    # ---- regions with roles
    text_by_region, vfx_by_region, spawn_regions = {}, {}, set()
    events = [e for lst in msb.get_all_subtype_lists() for e in lst if type(e).__name__.endswith("Event")]
    for ev in events:
        sub = type(ev).__name__
        reg = getattr(ev, "attached_region", None)
        if sub == "MSBMessageEvent" and reg is not None:
            text_by_region[reg.name] = int(ev.text_id)
        elif sub == "MSBVFXEvent" and reg is not None:
            vfx_by_region[reg.name] = int(ev.vfx_id)
        elif sub == "MSBSpawnPointEvent" and ev.spawn_point_region is not None:
            spawn_regions.add(ev.spawn_point_region.name)
    reg_index = {}
    for r in msb.regions:
        name = r.name
        ent = int(r.entity_id) if r.entity_id is not None else -1
        role = ROLE_BY_ENTITY.get(ent)
        if role is None:
            role = next((ro for k, ro in ROLE_BY_NAME if k in name), None)
        if role is None and name in spawn_regions:
            role = "RESPAWN"
        if name.startswith("navimesh"):
            continue
        sh = type(r.shape).__name__.replace("Shape", "").lower()
        size = [0.0, 0.0, 0.0]
        if sh == "box":
            size = [float(r.shape.width), float(r.shape.height), float(r.shape.depth)]
        elif sh == "sphere":
            size = [float(r.shape.radius), 0, 0]
        elif sh == "cylinder":
            size = [float(r.shape.radius), float(r.shape.height), 0]
        pos_fs = np.array([float(v) for v in r.translate])
        if cut_pit(pos_fs) and role not in ("FLOOR_COLLAPSE",):
            continue
        param = text_by_region.get(name, vfx_by_region.get(name, -1))
        if role == "RESPAWN" and "篝火復活ポイント" in name:
            param = 1811960 + int(name[-2:])   # 篝火復活ポイント00/01 -> bonfire objects 1811960/1811961 [версия]
        reg_index[name] = len(zone.regs)
        zone.regs.append(dict(entity=ent, shape=SHAPES.get(sh, 0), role=ROLES[role or "OTHER"],
                              pos=(pos_fs * MIRROR).tolist(), yaw=-math.radians(float(r.rotate[1])), size=size,
                              param=param, name=name, role_name=role or "OTHER"))
    # treasures: one region per treasure event, at its corpse object
    objs_by_name = {o.name: o for o in msb.objects}
    for ev in events:
        if type(ev).__name__ != "MSBTreasureEvent":
            continue
        tp = getattr(ev, "treasure_part", None)
        if tp is None or tp.name not in objs_by_name:
            continue
        pos_fs = np.array([float(v) for v in objs_by_name[tp.name].translate])
        if cut_pit(pos_fs):
            continue
        zone.regs.append(dict(entity=int(objs_by_name[tp.name].entity_id), shape=0, role=ROLES["TREASURE"],
                              pos=(pos_fs * MIRROR).tolist(), yaw=0.0, size=[0, 0, 0], param=int(ev.item_lot_1),
                              name=tp.name, role_name="TREASURE"))
    for c in zone.chrs:
        c["patrol"] = [reg_index.get(n, -1) for n in c.pop("patrol_names")] + [-1, -1]
        c["patrol"] = c["patrol"][:2]
    zone.start_reg = next(i for i, r in enumerate(zone.regs) if r["role_name"] == "START")

    # ---- write
    all_batches = sky + world
    nworld = len(all_batches)
    ashw, wst = write_ashw(bank.items, all_batches + mesh_batches,
                           [(s + nworld, n, r) for s, n, r in meshes], chunk=cfg["chunk"])
    ashz = write_ashz(zone)
    pak = write_pak([("world", ashw), ("pcol", pcol), ("ents", ashz)])
    out = Path(args.out).expanduser()
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(pak)
    # visibility per collision cell: triangles whose batch mask meets the cell's display groups
    vis = {}
    wm = [(np.array(b.mask, np.uint64), b.ntri if hasattr(b, "ntri") else len(b.tri)) for b in world]
    for c in zone.cols:
        d = np.array(c["disp"], np.uint64)
        n = sum(t for m, t in wm if (m & d).any())
        vis[c["name"]] = int(n)
    stats["world"] = wst
    stats["world"]["world_batches"] = len(world)
    stats["world"]["world_tris"] = int(sum(len(b.tri) for b in world))
    stats["world"]["sky_batches"] = len(sky)
    stats["visible_tris_per_cell"] = vis
    stats["entities"] = dict(cols=len(zone.cols), objs=len(zone.objs), chrs=len(zone.chrs), regs=len(zone.regs),
                             start=zone.regs[zone.start_reg]["pos"], bytes=len(ashz),
                             roles={r["role_name"]: sum(1 for x in zone.regs if x["role_name"] == r["role_name"])
                                    for r in zone.regs})
    stats["pak"] = dict(path=str(out), bytes=len(pak), mb=round(len(pak) / 2**20, 2), world=len(ashw), pcol=len(pcol),
                        ents=len(ashz))
    stats["textures"] = [dict(key=str(t.src[0]), src=t.src[1], cooked=(t.w, t.h), psm=t.psm, alpha=t.alpha)
                         for t in bank.items]
    stats["cook_seconds"] = round(time.time() - t0, 1)
    dump = dict(regions=[{k: (round(v, 2) if isinstance(v, float) else v) for k, v in r.items()} for r in zone.regs],
                chrs=[{k: v for k, v in c.items() if k != "mask"} for c in zone.chrs],
                objs=[dict(name=o["name"], entity=o["entity"], kind=o["kind"], mesh=o["mesh"], pos=list(map(float, o["pos"])),
                           boxes=o["boxes"]) for o in zone.objs],
                cols=[dict(name=c["name"], disp=c["disp"], cam_row=c["cam_row"]) for c in zone.cols])
    out.with_suffix(".json").write_text(json.dumps(stats, indent=1, default=str, ensure_ascii=False))
    out.with_suffix(".ents.json").write_text(json.dumps(dump, indent=1, default=str, ensure_ascii=False))
    s = {k: v for k, v in stats.items() if k not in ("textures", "map")}
    s["map"] = {k: v for k, v in stats["map"].items() if k != "pieces"}
    print(json.dumps(s, indent=1, default=str, ensure_ascii=False))
    if len(pak) > args.max_pak_mb * 2**20:
        sys.exit(f"FAIL: pak {len(pak) / 2**20:.2f} MB > {args.max_pak_mb} MB")


if __name__ == "__main__":
    main()
