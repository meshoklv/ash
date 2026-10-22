#!/usr/bin/env python3
"""Cook the player knight (c0000 + Elite Knight set + Longsword + Knight Shield) for hardware GE skinning,
plus its animation clips.

    tools/.venv/bin/python tools/cook_knight.py --out-dir ~/ash-assets/cooked

Outputs (never committed):
  knight.ashc     skinned mesh: reduced skeleton (<= 32 bones, inverse bind matrices), CLUT8 textures,
                  chunks of <= 8 bones (GE bone-matrix palette), vertices with 8 u8 weights summing to 128
  knight.ashanim  clips: 30 Hz keys, s16 quaternions (x,y,z,w * 32767), s16 translations (const tracks stored once),
                  root motion (x, y, z, yaw) per frame as a separate f32 track
  knight.json     stats + unit-test results

Skeleton reduction: every FLVER bone maps to itself if kept, else to its nearest kept ancestor (fingers, weapon
cases, dummies, nubs collapse into the hand / pelvis / spine). Vertex weights are summed per kept bone, the
4 largest kept (< 4/128 dropped), renormalised to exactly 128.

Space: FromSoft data is left-handed; everything is mirrored (x -> -x) like tools/cook_scene.py.
For matrices M' = S M S, S = diag(-1, 1, 1); quaternions (x, y, z, w) -> (x, -y, -z, w); root yaw -> -yaw.
"""
from __future__ import annotations

import argparse
import json
import logging
import math
import struct
import sys
import time
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).parent))
logging.disable(logging.WARNING)
from ashm import BF_ALPHA, BF_NOCULL, Batch, simplify, weld  # noqa: E402
from cook_scene import TexBank  # noqa: E402
from dsrlib import DSR, Binder, binder_flver, binder_textures, mesh_arrays, mesh_textures, mesh_triangles, valid_meshes  # noqa: E402
from pose import ANIBNDS, _entry, quat_to_mat  # noqa: E402

# Kept bones (<= 32). Twist bones are kept: they carry large weights on this armour (RCalfTwist ~ as much as the
# calf) and the budget allows it. `--merge-twist` folds them into their parents instead (26 bones).
KEEP = ["Master", "Pelvis",
        "L_Thigh", "L_Calf", "L_Foot", "L_Toe0", "LCalfTwist",
        "R_Thigh", "R_Calf", "R_Foot", "R_Toe0", "RCalfTwist",
        "Spine", "Spine1", "Neck", "Head",
        "L_Clavicle", "L_UpperArm", "L_Forearm", "L_Hand", "L_Weapon", "L_ForeTwist", "LUpArmTwist",
        "R_Clavicle", "R_UpperArm", "R_Forearm", "R_Hand", "R_Weapon", "R_ForeTwist", "RUpArmTwist"]
TWIST = {"LCalfTwist", "RCalfTwist", "L_ForeTwist", "R_ForeTwist", "LUpArmTwist", "RUpArmTwist"}

# Clip ids: MoveParam row 5 (中量 = medium weight): stand 0, walk 300, run 550 (DS1 "run" = our jog),
# sprint 760, roll forward 710, backstep 690; Longsword EquipParamWeapon.AttackAnimationCategory = 23 ->
# one-handed R1 #1 = a23_3000.
# Lock-on (MoveParam row 5): walk back/left/right 301/302/303, run (= our jog) back/left/right 551/552/553,
# rolls back/left/right 711/712/713.
CLIPS = [("idle", "a00_0000", True), ("walk", "a00_0300", True), ("jog", "a00_0550", True),
         ("run", "a00_0760", True), ("roll", "a00_0710", False), ("backstep", "a00_0690", False),
         ("attack", "a23_3000", False),
         ("walk_b", "a00_0301", True), ("walk_l", "a00_0302", True), ("walk_r", "a00_0303", True),
         ("jog_b", "a00_0551", True), ("jog_l", "a00_0552", True), ("jog_r", "a00_0553", True),
         ("roll_b", "a00_0711", False), ("roll_l", "a00_0712", False), ("roll_r", "a00_0713", False)]

PARTS = (("HD_A_9500", 0.12), ("BD_A_9500", 0.38), ("AM_A_9500", 0.2), ("LG_M_9500", 0.22),
         ("WP_A_0201", 0.03), ("WP_A_1516", 0.05))

S = np.diag([-1.0, 1.0, 1.0, 1.0])
T_SCALE = 4096.0          # translation units per metre (s16 -> +-8 m, 0.24 mm step)
MAX_PAL = 8               # GE bone matrices per draw call
MAX_INF = 4               # influences kept per vertex


def affine(t, R):
    M = np.eye(4)
    M[:3, :3] = R
    M[:3, 3] = t
    return M


def mat_to_quat(R: np.ndarray) -> np.ndarray:
    """Rotation matrix -> unit quaternion (x, y, z, w)."""
    m = R
    tr = m[0, 0] + m[1, 1] + m[2, 2]
    if tr > 0:
        s = math.sqrt(tr + 1.0) * 2
        q = [(m[2, 1] - m[1, 2]) / s, (m[0, 2] - m[2, 0]) / s, (m[1, 0] - m[0, 1]) / s, 0.25 * s]
    elif m[0, 0] > m[1, 1] and m[0, 0] > m[2, 2]:
        s = math.sqrt(1.0 + m[0, 0] - m[1, 1] - m[2, 2]) * 2
        q = [0.25 * s, (m[0, 1] + m[1, 0]) / s, (m[0, 2] + m[2, 0]) / s, (m[2, 1] - m[1, 2]) / s]
    elif m[1, 1] > m[2, 2]:
        s = math.sqrt(1.0 + m[1, 1] - m[0, 0] - m[2, 2]) * 2
        q = [(m[0, 1] + m[1, 0]) / s, 0.25 * s, (m[1, 2] + m[2, 1]) / s, (m[0, 2] - m[2, 0]) / s]
    else:
        s = math.sqrt(1.0 + m[2, 2] - m[0, 0] - m[1, 1]) * 2
        q = [(m[0, 2] + m[2, 0]) / s, (m[1, 2] + m[2, 1]) / s, 0.25 * s, (m[1, 0] - m[0, 1]) / s]
    q = np.array(q)
    return q / np.linalg.norm(q)


# --------------------------------------------------------------------------- skeleton


class Skeleton:
    def __init__(self, keep: list[str], chr_id: str = "c0000"):
        f = binder_flver(Binder.from_path(DSR / "chr" / f"{chr_id}.chrbnd.dcx"))
        self.names = [b.name for b in f.bones]
        self.parent = [b.parent_bone_index for b in f.bones]
        self.bind = [affine(np.array(list(t)), np.array(r.data)) for t, r, s in
                     f.get_bone_tree().get_bone_armature_space_transforms()]
        idx = {n: i for i, n in enumerate(self.names)}
        self.idx = idx
        # kept bones, ordered so that parents come first (FLVER order already is)
        self.keep = sorted(keep, key=lambda n: idx[n])
        self.kidx = {n: k for k, n in enumerate(self.keep)}
        self.kparent = []
        for n in self.keep:
            p = self.parent[idx[n]]
            while p >= 0 and self.names[p] not in self.kidx:
                p = self.parent[p]
            self.kparent.append(self.kidx[self.names[p]] if p >= 0 else -1)
        # any FLVER bone -> kept bone (itself or nearest kept ancestor; root-less dummies -> Master)
        self.to_kept = {}
        for i, n in enumerate(self.names):
            j = i
            while j >= 0 and self.names[j] not in self.kidx:
                j = self.parent[j]
            self.to_kept[n] = self.kidx[self.names[j]] if j >= 0 else 0
        self.kbind = [self.bind[idx[n]] for n in self.keep]

    def merged_into(self) -> dict[str, list[str]]:
        out: dict[str, list[str]] = {n: [] for n in self.keep}
        for n, k in self.to_kept.items():
            if n not in self.kidx:
                out[self.keep[k]].append(n)
        return out


# --------------------------------------------------------------------------- animation


def load_hkx_skeleton(chr_id: str = "c0000"):
    from soulstruct.havok.fromsoft.darksouls1r import SkeletonHKX
    return SkeletonHKX.from_bytes(_entry(Binder.from_path(DSR / "chr" / f"{chr_id}.anibnd.dcx"), "Skeleton.hkx"))


_BND_CACHE: dict[str, Binder] = {}


def find_anim(anim: str, bnds=None):
    for name in bnds or ANIBNDS:
        if name not in _BND_CACHE:
            try:
                _BND_CACHE[name] = Binder.from_path(DSR / "chr" / f"{name}.anibnd.dcx")
            except FileNotFoundError:
                continue
        try:
            return _entry(_BND_CACHE[name], f"{anim}.hkx"), name
        except KeyError:
            continue
    raise KeyError(anim)


def trs_mat(trs) -> np.ndarray:
    t = np.array(list(trs.translation), np.float64)
    q = np.array(trs.rotation._data if getattr(trs.rotation, "_data", None) is not None else list(trs.rotation),
                 np.float64)
    return affine(t, quat_to_mat(q / np.linalg.norm(q)))


def cook_clip(sk: Skeleton, hsk, anim: str, bnds=None) -> dict:
    """-> dict(frames, rot (F,K,4) f64 mirrored, trans (F,K,3) mirrored, root (F,4) mirrored, arma (F,K,4,4))."""
    from soulstruct.havok.fromsoft.darksouls1r import AnimationHKX
    data, src = find_anim(anim, bnds)
    cont = AnimationHKX.from_bytes(data).animation_container
    rm = None
    try:
        rm = np.array(cont.get_reference_frame_samples(), np.float64)
    except (ValueError, TypeError):
        pass
    if cont.is_spline:
        cont = cont.to_interleaved_container()
    arm = cont.get_interleaved_data_in_armature_space(hsk.skeleton)
    hk_names = [b.name for b in hsk.skeleton.bones]
    track_of = {hk_names[b]: ti for ti, b in enumerate(cont.get_track_bone_indices())}
    F, K = len(arm), len(sk.keep)
    A = np.zeros((F, K, 4, 4))
    for k, n in enumerate(sk.keep):
        ti = track_of.get(n)
        for f in range(F):
            A[f, k] = trs_mat(arm[f][ti]) if ti is not None else sk.kbind[k]
    rot = np.zeros((F, K, 4))
    trans = np.zeros((F, K, 3))
    for f in range(F):
        for k in range(K):
            p = sk.kparent[k]
            L = A[f, k] if p < 0 else np.linalg.inv(A[f, p]) @ A[f, k]
            Lm = S @ L @ S
            rot[f, k] = mat_to_quat(Lm[:3, :3])
            trans[f, k] = Lm[:3, 3]
    # hemisphere continuity so the runtime can nlerp neighbours without a sign test
    for f in range(1, F):
        flip = (rot[f] * rot[f - 1]).sum(1) < 0
        rot[f, flip] *= -1
    root = np.zeros((F, 4))
    if rm is not None and len(rm):
        n = min(F, len(rm))
        root[:n] = rm[:n]
        root[n:] = rm[n - 1]
        root[:, 0] *= -1   # mirror x
        root[:, 3] *= -1   # mirror yaw
    return dict(src=f"{src}/{anim}", frames=F, rot=rot, trans=trans, root=root, arma=A)


def quantize_clip(c: dict) -> dict:
    q = np.clip(np.round(c["rot"] * 32767), -32767, 32767).astype(np.int16)
    t = np.clip(np.round(c["trans"] * T_SCALE), -32767, 32767).astype(np.int16)
    F, K = q.shape[:2]
    animated = np.zeros(K, bool)
    for k in range(K):
        animated[k] = np.abs(c["trans"][:, k] - c["trans"][0, k]).max() > 0.5e-3
    tstream, tstart = [], []
    pos = 0
    for k in range(K):
        tstart.append(pos)
        if animated[k]:
            tstream.append(t[:, k]); pos += F
        else:
            tstream.append(t[:1, k]); pos += 1
    return dict(q=q, t=np.concatenate(tstream), tstart=np.array(tstart), animated=animated)


def reconstruct(sk: Skeleton, qc: dict, F: int) -> np.ndarray:
    """Armature (mirrored) matrices from the quantized data, the way the PSP runtime does it."""
    K = len(sk.keep)
    out = np.zeros((F, K, 4, 4))
    for f in range(F):
        for k in range(K):
            q = qc["q"][f, k].astype(np.float64) / 32767.0
            q /= np.linalg.norm(q)
            ti = qc["tstart"][k] + (f if qc["animated"][k] else 0)
            L = affine(qc["t"][ti].astype(np.float64) / T_SCALE, quat_to_mat(q))
            p = sk.kparent[k]
            out[f, k] = L if p < 0 else out[f, p] @ L
    return out


# --------------------------------------------------------------------------- mesh


def part_bone_to_kept(sk: Skeleton, pf) -> list[int]:
    names = [b.name for b in pf.bones]
    out = []
    for i, n in enumerate(names):
        j = i
        while j >= 0 and names[j] not in sk.to_kept:
            j = pf.bones[j].parent_bone_index
        out.append(sk.to_kept[names[j]] if j >= 0 else 0)
    return out


def cook_mesh(sk: Skeleton, bank: TexBank, budget: int, stats: dict) -> list[Batch]:
    K = len(sk.keep)
    out = []
    src = 0
    for part, share in PARTS:
        b = Binder.from_path(DSR / "parts" / f"{part}.partsbnd.dcx")
        pf = binder_flver(b)
        tx = binder_textures(b)
        p2k = part_bone_to_kept(sk, pf)
        parts = []
        for m in valid_meshes(pf):
            dstem = mesh_textures(m).get("g_Diffuse")
            if not dstem or dstem not in tx:
                continue
            d = mesh_arrays(m)
            tri = mesh_triangles(m)
            src += len(tri)
            pos = d["pos"].astype(np.float64)
            nrm = d.get("normal", np.tile([0, 1, 0], (len(pos), 1)))[:, :3].astype(np.float64)
            W = np.zeros((len(pos), K), np.float32)
            if not m.is_dynamic:
                # rigid (weapons): part bone bind, then the attach bone's bind (as tools/cook_scene.py does)
                bi = m.default_bone_index if m.default_bone_index >= 0 else 0
                bt = pf.get_bone_tree().get_bone_armature_space_transforms()[bi]
                Rb = np.array(bt[1].data); tb = np.array(list(bt[0]))
                pos = pos @ Rb.T + tb; nrm = nrm @ Rb.T
                attach = "R_Weapon" if part.startswith("WP_A_02") else "L_Weapon" if part.startswith("WP_A_15") else None
                if attach:
                    A = sk.bind[sk.idx[attach]]
                    pos = pos @ A[:3, :3].T + A[:3, 3]; nrm = nrm @ A[:3, :3].T
                    W[:, sk.kidx[attach]] = 1
                else:
                    W[:, p2k[bi]] = 1
            else:
                bi = d["bone_indices"]
                bw = d.get("bone_weights")
                if bw is None:
                    bw = np.zeros(bi.shape, np.float32); bw[:, 0] = 1
                lut = np.array(m.bone_indices) if len(m.bone_indices) else None
                for kk in range(bi.shape[1]):
                    idx = bi[:, kk]
                    g = lut[np.clip(idx, 0, len(lut) - 1)] if lut is not None else idx
                    kb = np.array(p2k)[np.clip(g, 0, len(p2k) - 1)]
                    np.add.at(W, (np.arange(len(pos)), kb), bw[:, kk])
                zero = W.sum(1) < 1e-6
                W[zero, 0] = 1
            # mirror to PSP space
            pos[:, 0] *= -1; nrm[:, 0] *= -1
            ti = bank.add(("knight", dstem), lambda: tx[dstem][0], 128)
            flags = BF_ALPHA | BF_NOCULL if bank.items[ti].alpha else 0
            parts.append(Batch(ti, pos.astype(np.float32), d["uv0"].astype(np.float32), nrm.astype(np.float32), tri,
                               flags, f"{part}:{dstem}", W))
        total = sum(p.ntri for p in parts)
        ratio = min(1.0, budget * share / max(1, total))
        for p in parts:
            s = simplify(weld(p), max(8, int(p.ntri * ratio)))
            s = unify_corners(s)
            n = s.col / (np.linalg.norm(s.col, axis=1, keepdims=True) + 1e-9)
            s.col = n.astype(np.float32)
            out.append(s)
    stats["mesh"] = dict(src_tris=src, tris=sum(b.ntri for b in out), verts_before_chunking=sum(len(b.pos) for b in out))
    return out


def unify_corners(b: Batch) -> Batch:
    """simplify() re-projects attributes per triangle corner, so corners sharing a position+UV get slightly
    different normals/weights and do not weld. Average them per (position, UV) and weld again."""
    key = np.concatenate([np.round(b.pos / 1e-4), np.round(b.uv * 256)], 1).astype(np.int64)  # UV within ~1/256
    _, inv = np.unique(key, axis=0, return_inverse=True)
    inv = inv.reshape(-1)
    n = inv.max() + 1
    cnt = np.bincount(inv, minlength=n)[:, None]
    uv = np.zeros((n, 2)); np.add.at(uv, inv, b.uv)
    b.uv = (uv / cnt)[inv].astype(np.float32)
    col = np.zeros((n, 3)); np.add.at(col, inv, b.col)
    attr = np.zeros((n, b.attr.shape[1])); np.add.at(attr, inv, b.attr)
    b.col = (col / cnt)[inv].astype(np.float32)
    b.attr = (attr / cnt)[inv].astype(np.float32)
    return weld(b)


def quantize_weights(W: np.ndarray) -> np.ndarray:
    """(N,K) float -> (N,K) int, <= MAX_INF non-zero, each row sums to 128."""
    N, K = W.shape
    out = np.zeros((N, K), np.int32)
    for i in range(N):
        w = W[i] / max(1e-9, W[i].sum())
        top = np.argsort(-w)[:MAX_INF]
        top = [j for j in top if w[j] >= 4 / 128.0] or [int(np.argmax(w))]
        v = w[top] / w[top].sum() * 128
        q = np.floor(v).astype(int)
        rem = 128 - q.sum()
        for j in np.argsort(-(v - q))[:rem]:
            q[j] += 1
        out[i, top] = q
    return out


def chunk_batch(b: Batch, Wq: np.ndarray):
    """Split a batch into chunks whose triangles reference <= MAX_PAL bones.
    -> list of (palette, vert_idx (V,), tri (T,3) local, weights (V,8) u8 in palette order)."""
    tri_bones = [set(np.nonzero(Wq[t].sum(0))[0].tolist()) for t in b.tri]
    order = sorted(range(len(b.tri)), key=lambda i: (-len(tri_bones[i]), sorted(tri_bones[i])))
    chunks: list[dict] = []
    for ti in order:
        bs = tri_bones[ti]
        if len(bs) > MAX_PAL:  # keep the 8 heaviest bones of this triangle, renormalised per chunk vertex below
            tot = Wq[b.tri[ti]].sum(0)
            bs = set(np.argsort(-tot)[:MAX_PAL].tolist())
        best, best_grow = None, 99
        for c in chunks:
            grow = len(bs - c["pal"])
            if len(c["pal"]) + grow <= MAX_PAL and grow < best_grow:
                best, best_grow = c, grow
                if grow == 0:
                    break
        if best is None:
            best = dict(pal=set(), tris=[])
            chunks.append(best)
        best["pal"] |= bs
        best["tris"].append(ti)
    out = []
    for c in chunks:
        pal = sorted(c["pal"])
        tris = b.tri[c["tris"]]
        used, inv = np.unique(tris, return_inverse=True)
        w = np.zeros((len(used), MAX_PAL), np.int32)
        for slot, bone in enumerate(pal):
            w[:, slot] = Wq[used, bone]
        # out-of-palette weight (only for > 8-bone triangles): renormalise to 128
        for i in np.nonzero(w.sum(1) != 128)[0]:
            v = w[i].astype(np.float64)
            if v.sum() == 0:
                v[0] = 1
            v = v / v.sum() * 128
            q = np.floor(v).astype(int)
            for j in np.argsort(-(v - q))[:128 - q.sum()]:
                q[j] += 1
            w[i] = q
        out.append((pal, used, inv.reshape(-1, 3), w.astype(np.uint8)))
    return out


# --------------------------------------------------------------------------- writers

VERT_DTYPE = np.dtype([("w", "u1", 8), ("uv", "<f4", 2), ("n", "i1", 3), ("pad", "u1"), ("pos", "<f4", 3)])
assert VERT_DTYPE.itemsize == 32


def _align(buf: bytearray, a=16):
    while len(buf) % a:
        buf.append(0)


def write_ashc(path: Path, sk: Skeleton, bank: TexBank, chunks: list) -> dict:
    """.ashc v1, little-endian, sections 16-aligned, offsets from file start.
    Header 64 B: "ASHC", u32 version=1, n_bone, n_tex, n_chunk, n_vert, n_idx, flags,
                 off_bone, off_tex, off_chunk, off_vert, off_idx, f32 radius, u32 pad[2]
    Bone 80 B:  i32 parent, char name[28], f32 inv_bind[4][3] (column-major 4x3: x axis, y axis, z axis, translation)
    Tex 16 B:   as .ashm (u16 w, h, psm=5, flags; u32 off_clut, off_pix)
    Chunk 32 B: u16 tex, u16 flags, u8 n_pal, u8 pad[3], u8 pal[8], u32 vert_start, vert_count, idx_start, idx_count
    Vertex 32 B: u8 w[8] (sum 128, slot i -> pal[i]); f32 u, v; s8 nx, ny, nz, pad; f32 x, y, z
    Index u16, relative to the chunk's vert_start."""
    body = bytearray(b"\0" * 64)
    off_bone = len(body)
    for k, n in enumerate(sk.keep):
        ib = S @ np.linalg.inv(sk.kbind[k]) @ S
        cm = [ib[r, c] for c in range(4) for r in range(3)]
        nm = n.encode("ascii", "replace")[:27]
        body += struct.pack("<i28s12f", sk.kparent[k], nm, *cm)
    _align(body)
    off_tex = len(body)
    body += b"\0" * (16 * len(bank.items)); _align(body)
    ents = []
    for t in bank.items:
        _align(body); oc = len(body); body += t.clut
        _align(body); op = len(body); body += t.pix
        ents.append(struct.pack("<4H2I", t.w, t.h, 5, 1 if t.alpha else 0, oc, op))
    body[off_tex:off_tex + 16 * len(ents)] = b"".join(ents)
    _align(body)
    recs, verts, idxs = [], [], []
    vs = is_ = 0
    for tex, flags, pal, pos, uv, nrm, w, tri in chunks:
        v = np.zeros(len(pos), VERT_DTYPE)
        v["w"] = w; v["uv"] = uv; v["pos"] = pos
        v["n"] = np.clip(np.round(nrm * 127), -127, 127).astype(np.int8)
        verts.append(v); idxs.append(tri.astype(np.uint16).reshape(-1))
        palb = bytes(pal) + b"\0" * (8 - len(pal))
        recs.append(struct.pack("<2HB3x8s4I", tex, flags, len(pal), palb, vs, len(pos), is_, tri.size))
        vs += len(pos); is_ += tri.size
    off_chunk = len(body); body += b"".join(recs); _align(body)
    off_vert = len(body); body += b"".join(v.tobytes() for v in verts); _align(body)
    off_idx = len(body); body += b"".join(i.tobytes() for i in idxs); _align(body)
    allp = np.concatenate([c[3] for c in chunks])
    radius = float(np.linalg.norm(allp - allp.mean(0), axis=1).max())
    hdr = struct.pack("<4s12If2I", b"ASHC", 1, len(sk.keep), len(bank.items), len(chunks), vs, is_, 0,
                      off_bone, off_tex, off_chunk, off_vert, off_idx, radius, 0, 0)
    body[:len(hdr)] = hdr
    path.write_bytes(body)
    return dict(bytes=len(body), bones=len(sk.keep), tex=len(bank.items), chunks=len(chunks), verts=vs,
                tris=is_ // 3, tex_bytes=sum(len(t.clut) + len(t.pix) for t in bank.items))


def write_ashanim(path: Path, n_bones: int, clips: list) -> dict:
    """.ashanim v1, little-endian, sections 16-aligned.
    Header 32 B: "ASHA", u32 version=1, u32 n_bones, u32 n_clips, f32 t_scale (units per metre), f32 fps,
                 u32 off_clips, u32 pad
    Clip 64 B:   char name[16], u32 anim_id (XX * 10000 + YYYY, e.g. 233000 for a23_3000), u16 n_frames, u16 flags (1 = loop),
                 u32 off_rot   (n_frames * n_bones * s16[4], frame-major, x y z w * 32767)
                 u32 off_tstart (n_bones * u16: first index into the translation stream)
                 u32 off_tflag  (n_bones * u8: 1 = one key per frame, 0 = constant)
                 u32 off_trans  (s16[3] stream, metres * t_scale)
                 u32 off_root   (n_frames * f32[4]: x, y, z, yaw - cumulative from frame 0, character space)
                 u32 n_trans, f32 root_dist (|xz| at the last frame), u32 pad[3]"""
    body = bytearray(b"\0" * 32)
    off_clips = len(body)
    body += b"\0" * (64 * len(clips)); _align(body)
    recs = []
    for name, aid, loop, c, qc in clips:
        _align(body); o_rot = len(body); body += qc["q"].astype("<i2").tobytes()
        _align(body); o_ts = len(body); body += qc["tstart"].astype("<u2").tobytes()
        _align(body); o_tf = len(body); body += qc["animated"].astype("u1").tobytes()
        _align(body); o_tr = len(body); body += qc["t"].astype("<i2").tobytes()
        _align(body); o_rt = len(body); body += c["root"].astype("<f4").tobytes()
        dist = float(np.hypot(c["root"][-1, 0], c["root"][-1, 2]))
        recs.append(struct.pack("<16sIHH6If3I", name.encode()[:15], aid, c["frames"], 1 if loop else 0,
                                o_rot, o_ts, o_tf, o_tr, o_rt, len(qc["t"]), dist, 0, 0, 0))
    body[off_clips:off_clips + 64 * len(recs)] = b"".join(recs)
    _align(body)
    body[:32] = struct.pack("<4s3I2f2I", b"ASHA", 1, n_bones, len(clips), T_SCALE, 30.0, off_clips, 0)
    path.write_bytes(body)
    return dict(bytes=len(body))


# --------------------------------------------------------------------------- main


def anim_id(a: str) -> int:
    x, y = a[1:].split("_")
    return int(x) * 10000 + int(y) if len(y) == 4 else int(x) * 1000000 + int(y)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out-dir", required=True)
    ap.add_argument("--budget", type=int, default=3000, help="knight triangle budget")
    ap.add_argument("--merge-twist", action="store_true")
    args = ap.parse_args()
    t0 = time.time()
    out = Path(args.out_dir).expanduser()
    out.mkdir(parents=True, exist_ok=True)
    keep = [n for n in KEEP if not (args.merge_twist and n in TWIST)]
    sk = Skeleton(keep)
    stats: dict = dict(skeleton=dict(flver_bones=len(sk.names), kept=len(sk.keep), keep=sk.keep,
                                     merged={k: v for k, v in sk.merged_into().items() if v}))
    assert len(sk.keep) <= 32
    # ---- animation
    hsk = load_hkx_skeleton()
    stats["skeleton"]["hkx_bones"] = len(hsk.skeleton.bones)
    clips = []
    tests = {}
    for name, a, loop in CLIPS:
        c = cook_clip(sk, hsk, a)
        qc = quantize_clip(c)
        rec = reconstruct(sk, qc, c["frames"])
        ref = np.einsum("ij,fkjl,lm->fkim", S, c["arma"], S)
        err = np.abs(rec[:, :, :3, 3] - ref[:, :, :3, 3]).max()
        tests[f"{name}_max_joint_err_mm"] = round(float(err) * 1000, 3)
        clips.append((name, anim_id(a), loop, c, qc))
        print(f"clip {name:7s} {c['src']:24s} frames={c['frames']:3d} root_end={c['root'][-1].round(3).tolist()} "
              f"animated_trans={int(qc['animated'].sum())}/{len(sk.keep)} joint_err={err*1000:.2f} mm", flush=True)
    stats["clips"] = [dict(name=n, id=a, src=c["src"], frames=c["frames"], loop=lp,
                           root_end=[round(float(x), 3) for x in c["root"][-1]],
                           animated_translation_bones=int(qc["animated"].sum())) for n, a, lp, c, qc in clips]
    stats["ashanim"] = write_ashanim(out / "knight.ashanim", len(sk.keep), clips)
    # ---- mesh
    bank = TexBank(128)
    batches = cook_mesh(sk, bank, args.budget, stats)
    chunks = []
    max_bad = 0
    for b in batches:
        Wq = quantize_weights(b.attr)
        for pal, used, tri, w in chunk_batch(b, Wq):
            chunks.append((b.tex, b.flags, pal, b.pos[used], b.uv[used], b.col[used], w, tri))
            s = w.astype(int).sum(1)
            max_bad = max(max_bad, int(np.abs(s - 128).max()))
    # ---- unit tests (PLAN §3.6)
    tests["weights_sum_128_max_dev"] = max_bad
    tests["palette_le_8"] = all(len(c[2]) <= MAX_PAL for c in chunks)
    # weights outside the palette cannot exist by construction (slots == palette); check slot count
    tests["no_weight_outside_palette"] = all((c[6][:, len(c[2]):] == 0).all() for c in chunks)
    # bind pose: skin = world(bind) * inv_bind must give the vertex back
    kb = [S @ m @ S for m in sk.kbind]
    worst = 0.0
    for tex, flags, pal, pos, uv, nrm, w, tri in chunks:
        acc = np.zeros_like(pos, dtype=np.float64)
        for slot, bone in enumerate(pal):
            M = kb[bone] @ (S @ np.linalg.inv(sk.kbind[bone]) @ S)
            acc += (w[:, slot:slot + 1] / 128.0) * (pos @ M[:3, :3].T + M[:3, 3])
        worst = max(worst, float(np.abs(acc - pos).max()))
    tests["bind_pose_max_err_mm"] = round(worst * 1000, 4)
    stats["tests"] = tests
    stats["ashc"] = write_ashc(out / "knight.ashc", sk, bank, chunks)
    pals = [len(c[2]) for c in chunks]
    stats["chunks"] = dict(count=len(chunks), palette_hist={str(k): pals.count(k) for k in sorted(set(pals))})
    stats["cook_seconds"] = round(time.time() - t0, 1)
    (out / "knight.json").write_text(json.dumps(stats, indent=1, default=str))
    print(json.dumps({k: v for k, v in stats.items() if k not in ("clips",)}, indent=1, default=str))
    ok = (tests["weights_sum_128_max_dev"] == 0 and tests["palette_le_8"] and tests["no_weight_outside_palette"]
          and tests["bind_pose_max_err_mm"] < 1.0)
    print("UNIT TESTS:", "PASS" if ok else "FAIL")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
