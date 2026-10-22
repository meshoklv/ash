#!/usr/bin/env python3
"""Cook an enemy (FLVER + HKX clips from its own chrbnd/anibnd) for GE skinning, same formats as tools/cook_knight.py.

    tools/.venv/bin/python tools/cook_enemy.py --enemy hollow --out-dir ~/ash-assets/cooked
    tools/.venv/bin/python tools/cook_enemy.py --enemy hollow --probe     # list clips: frames, root motion, pelvis

Outputs (never committed): <name>.ashc, <name>.ashanim, <name>.json — byte layout identical to knight.* (see
cook_knight.write_ashc / write_ashanim), so the runtime that plays the knight plays the enemy.

Shared code is imported from cook_knight (quantisation, chunking, writers, unit tests) — cook_knight itself is not
modified. What differs: skeleton and clips come from c####.chrbnd / c####.anibnd, meshes are picked by the
NpcParam ModelDisplayMask (material name "#NN#" = mask bit NN; unnamed meshes always drawn) [версия: rule read
from c2500, where #00# = sword (mask0 of NpcParam 250022), #01# = torch, #02# = bow].

Enemy choice and clip ids: the ai-facts design notes §5.
"""
from __future__ import annotations

import argparse
import json
import logging
import re
import sys
import time
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).parent))
logging.disable(logging.WARNING)
from cook_knight import (MAX_PAL, S, affine, anim_id, chunk_batch, mat_to_quat, quantize_clip,  # noqa: E402
                         quantize_weights, reconstruct, trs_mat, unify_corners, write_ashanim, write_ashc)
from ashm import BF_ALPHA, BF_NOCULL, Batch, simplify, weld  # noqa: E402
from cook_scene import TexBank  # noqa: E402
from dsrlib import (DSR, Binder, binder_flver, chr_textures, mesh_arrays, mesh_textures,  # noqa: E402
                    mesh_triangles, valid_meshes)
from pose import _entry  # noqa: E402

ENEMIES = {
    # Asylum hollow with a broken straight sword: MSB m18_01 c2500_0002/0009/0010 -> NpcParam 250022
    # (チュート 不死人, HP 69, mask0 = sword), NpcThinkParam 250000 (不死人（剣）) -> battle goal 250000.
    "hollow": dict(
        chr="c2500", npc=250022, think=250000, budget=1800, mask={0},
        # body textures live in c2509.chrbnd = NpcParam.NormalChangeTextureChrID 2509
        tex_chrs=["c2500", "c2509"],
        keep=["Pelvis", "Spine", "Spine1", "Neck", "Head",
              "L Clavicle", "L UpperArm", "L Forearm", "L Hand", "L ForeTwist",
              "R Clavicle", "R UpperArm", "R Forearm", "R Hand", "sword", "R ForeTwist",
              "L Thigh", "L Calf", "L Foot", "L Toe0", "LCalfTwist",
              "R Thigh", "R Calf", "R Foot", "R Toe0", "RCalfTwist"],
        # MoveParam 100 (敵基本 戦闘時): stand 0, walk 200, run 500. Attacks: the two the battle script picks most
        # (ai_proto scenario A): 3000 (close, combo opener) and 3004 (mid, 2.5-4 m). Damage / death: see ai-facts §5.
        clips=[("idle", "a00_0000", True), ("walk", "a00_0200", True), ("run", "a00_0500", True),
               ("attack1", "a00_3000", False), ("attack2", "a00_3004", False),
               ("hit", "a00_2000", False), ("death", "a00_2200", False),
               # parried (TAE SpEffect 30 = riposte window) and the throw victims: src/game/asy_game.c
               ("parried", "a00_2100", False), ("riposted", "a00_2510", False), ("backstabbed", "a00_2500", False)],
    ),
}


class EnemySkeleton:
    """Same interface as cook_knight.Skeleton (keep, kparent, kbind, idx, to_kept, names) for any chr."""

    def __init__(self, chr_id: str, keep: list[str]):
        f = binder_flver(Binder.from_path(DSR / f"chr/{chr_id}.chrbnd.dcx"))
        self.flver = f
        self.names = [b.name for b in f.bones]
        self.parent = [b.parent_bone_index for b in f.bones]
        self.bind = [affine(np.array(list(t)), np.array(r.data)) for t, r, s in
                     f.get_bone_tree().get_bone_armature_space_transforms()]
        self.idx = {n: i for i, n in enumerate(self.names)}
        self.keep = sorted(keep, key=lambda n: self.idx[n])
        self.kidx = {n: k for k, n in enumerate(self.keep)}
        self.kparent = []
        for n in self.keep:
            p = self.parent[self.idx[n]]
            while p >= 0 and self.names[p] not in self.kidx:
                p = self.parent[p]
            self.kparent.append(self.kidx[self.names[p]] if p >= 0 else -1)
        self.to_kept = {}
        for i, n in enumerate(self.names):
            j = i
            while j >= 0 and self.names[j] not in self.kidx:
                j = self.parent[j]
            self.to_kept[n] = self.kidx[self.names[j]] if j >= 0 else 0
        self.kbind = [self.bind[self.idx[n]] for n in self.keep]

    def merged_into(self):
        out = {n: [] for n in self.keep}
        for n, k in self.to_kept.items():
            if n not in self.kidx:
                out[self.keep[k]].append(n)
        return out


def load_anibnd(chr_id):
    from soulstruct.havok.fromsoft.darksouls1r import SkeletonHKX
    b = Binder.from_path(DSR / "chr" / f"{chr_id}.anibnd.dcx")
    return b, SkeletonHKX.from_bytes(_entry(b, "Skeleton.hkx"))


def cook_clip(sk: EnemySkeleton, hsk, b: Binder, anim: str) -> dict:
    """As cook_knight.cook_clip, reading from the enemy's anibnd. Kept bones missing from the HKX keep bind pose."""
    from soulstruct.havok.fromsoft.darksouls1r import AnimationHKX
    cont = AnimationHKX.from_bytes(_entry(b, f"{anim}.hkx")).animation_container
    rm = None
    try:
        rm = np.array(cont.get_reference_frame_samples(), np.float64)
    except (ValueError, TypeError):
        pass
    if cont.is_spline:
        cont = cont.to_interleaved_container()
    arm = cont.get_interleaved_data_in_armature_space(hsk.skeleton)
    hk_names = [bn.name for bn in hsk.skeleton.bones]
    track_of = {hk_names[bi]: ti for ti, bi in enumerate(cont.get_track_bone_indices())}
    F, K = len(arm), len(sk.keep)
    A = np.zeros((F, K, 4, 4))
    missing = [n for n in sk.keep if n not in track_of]
    for k, n in enumerate(sk.keep):
        ti = track_of.get(n)
        for f in range(F):
            if ti is not None:
                A[f, k] = trs_mat(arm[f][ti])
            else:  # follow the kept parent with the bind-pose local offset
                p = sk.kparent[k]
                L = np.linalg.inv(sk.kbind[p]) @ sk.kbind[k] if p >= 0 else sk.kbind[k]
                A[f, k] = (A[f, p] @ L) if p >= 0 else L
    rot = np.zeros((F, K, 4))
    trans = np.zeros((F, K, 3))
    for f in range(F):
        for k in range(K):
            p = sk.kparent[k]
            L = A[f, k] if p < 0 else np.linalg.inv(A[f, p]) @ A[f, k]
            Lm = S @ L @ S
            rot[f, k] = mat_to_quat(Lm[:3, :3])
            trans[f, k] = Lm[:3, 3]
    for f in range(1, F):
        flip = (rot[f] * rot[f - 1]).sum(1) < 0
        rot[f, flip] *= -1
    root = np.zeros((F, 4))
    if rm is not None and len(rm):
        n = min(F, len(rm))
        root[:n] = rm[:n]
        root[n:] = rm[n - 1]
        root[:, 0] *= -1
        root[:, 3] *= -1
    return dict(src=f"{sk_chr(sk)}/{anim}", frames=F, rot=rot, trans=trans, root=root, arma=A, missing=missing)


def sk_chr(sk):
    return getattr(sk, "chr", "?")


MASK_RE = re.compile(r"#(\d+)#")


def cook_mesh(sk: EnemySkeleton, cfg: dict, bank: TexBank, budget: int, stats: dict) -> list[Batch]:
    f = sk.flver
    tx = {}
    for c in cfg.get("tex_chrs", [cfg["chr"]]):
        tx.update(chr_textures(c))
    K = len(sk.keep)
    f2k = [sk.to_kept[n] for n in sk.names]
    parts, used, skipped = [], [], []
    src = 0
    for mi, m in enumerate(valid_meshes(f)):
        mm = MASK_RE.search(m.material.name)
        if mm and int(mm.group(1)) not in cfg["mask"]:
            skipped.append(f"{mi}:{m.material.name}")
            continue
        dstem = mesh_textures(m).get("g_Diffuse")
        if not dstem or dstem not in tx:
            skipped.append(f"{mi}:{m.material.name}(no tex {dstem})")
            continue
        d = mesh_arrays(m)
        tri = mesh_triangles(m)
        src += len(tri)
        pos = d["pos"].astype(np.float64)
        nrm = d.get("normal", np.tile([0, 1, 0], (len(pos), 1)))[:, :3].astype(np.float64)
        W = np.zeros((len(pos), K), np.float32)
        if m.is_dynamic:
            bi = d["bone_indices"]
            bw = d.get("bone_weights")
            if bw is None:
                bw = np.zeros(bi.shape, np.float32); bw[:, 0] = 1
            lut = np.array(m.bone_indices) if len(m.bone_indices) else None
            for kk in range(bi.shape[1]):
                g = lut[np.clip(bi[:, kk], 0, len(lut) - 1)] if lut is not None else bi[:, kk]
                kb = np.array(f2k)[np.clip(g, 0, len(f2k) - 1)]
                np.add.at(W, (np.arange(len(pos)), kb), bw[:, kk])
            W[W.sum(1) < 1e-6, 0] = 1
        else:
            b0 = m.default_bone_index if m.default_bone_index >= 0 else 0
            A = sk.bind[b0]
            pos = pos @ A[:3, :3].T + A[:3, 3]; nrm = nrm @ A[:3, :3].T
            W[:, f2k[b0]] = 1
        pos[:, 0] *= -1; nrm[:, 0] *= -1
        ti = bank.add((cfg["chr"], dstem), lambda s=dstem: tx[s][0], 128)
        flags = BF_ALPHA | BF_NOCULL if bank.items[ti].alpha else 0
        parts.append(Batch(ti, pos.astype(np.float32), d["uv0"].astype(np.float32), nrm.astype(np.float32), tri,
                           flags, f"{cfg['chr']}:{mi}:{m.material.name}:{dstem}", W))
        used.append(f"{mi}:{m.material.name}:{dstem}:{len(tri)}tri")
    total = sum(p.ntri for p in parts)
    ratio = min(1.0, budget / max(1, total))
    out = []
    for p in parts:
        s = simplify(weld(p), max(8, int(p.ntri * ratio)))
        s = unify_corners(s)
        s.col = (s.col / (np.linalg.norm(s.col, axis=1, keepdims=True) + 1e-9)).astype(np.float32)
        out.append(s)
    stats["mesh"] = dict(src_tris=src, tris=sum(b.ntri for b in out), used=used, skipped=skipped)
    return out


def probe(cfg):
    sk = EnemySkeleton(cfg["chr"], cfg["keep"])
    sk.chr = cfg["chr"]
    b, hsk = load_anibnd(cfg["chr"])
    names = sorted(e.name.replace("\\", "/").rsplit("/", 1)[-1][:-4] for e in b.entries if e.name.endswith(".hkx")
                   and "Skeleton" not in e.name)
    pel = sk.kidx["Pelvis"]
    print(f"{cfg['chr']}: {len(names)} clips. frames | root end (x,y,z,yaw) | pelvis height first->last")
    for a in names:
        if not a.startswith("a00_"):
            continue
        try:
            c = cook_clip(sk, hsk, b, a)
        except Exception as ex:  # pragma: no cover
            print(f"  {a}: {ex}")
            continue
        h0, h1 = c["arma"][0, pel, 1, 3], c["arma"][-1, pel, 1, 3]
        print(f"  {a}  {c['frames']:4d}  {np.round(c['root'][-1], 2).tolist()}  {h0:.2f}->{h1:.2f}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--enemy", default="hollow", choices=sorted(ENEMIES))
    ap.add_argument("--out-dir")
    ap.add_argument("--budget", type=int)
    ap.add_argument("--probe", action="store_true")
    a = ap.parse_args()
    cfg = ENEMIES[a.enemy]
    if a.probe:
        probe(cfg)
        return
    t0 = time.time()
    out = Path(a.out_dir).expanduser()
    out.mkdir(parents=True, exist_ok=True)
    sk = EnemySkeleton(cfg["chr"], cfg["keep"])
    sk.chr = cfg["chr"]
    assert len(sk.keep) <= 32
    stats: dict = dict(enemy=a.enemy, chr=cfg["chr"], npc=cfg["npc"], think=cfg["think"],
                       skeleton=dict(flver_bones=len(sk.names), kept=len(sk.keep), keep=sk.keep,
                                     merged={k: v for k, v in sk.merged_into().items() if v}))
    b, hsk = load_anibnd(cfg["chr"])
    stats["skeleton"]["hkx_bones"] = len(hsk.skeleton.bones)
    clips, tests = [], {}
    for name, an, loop in cfg["clips"]:
        c = cook_clip(sk, hsk, b, an)
        qc = quantize_clip(c)
        rec = reconstruct(sk, qc, c["frames"])
        ref = np.einsum("ij,fkjl,lm->fkim", S, c["arma"], S)
        err = np.abs(rec[:, :, :3, 3] - ref[:, :, :3, 3]).max()
        tests[f"{name}_max_joint_err_mm"] = round(float(err) * 1000, 3)
        clips.append((name, anim_id(an), loop, c, qc))
        print(f"clip {name:8s} {c['src']:16s} frames={c['frames']:3d} root_end={c['root'][-1].round(3).tolist()} "
              f"joint_err={err*1000:.2f} mm missing_in_hkx={c['missing']}", flush=True)
    stats["clips"] = [dict(name=n, id=i, src=c["src"], frames=c["frames"], loop=lp,
                           root_end=[round(float(x), 3) for x in c["root"][-1]]) for n, i, lp, c, qc in clips]
    stats["ashanim"] = write_ashanim(out / f"{a.enemy}.ashanim", len(sk.keep), clips)
    bank = TexBank(128)
    batches = cook_mesh(sk, cfg, bank, a.budget or cfg["budget"], stats)
    chunks, max_bad = [], 0
    for bt in batches:
        Wq = quantize_weights(bt.attr)
        for pal, used, tri, w in chunk_batch(bt, Wq):
            chunks.append((bt.tex, bt.flags, pal, bt.pos[used], bt.uv[used], bt.col[used], w, tri))
            max_bad = max(max_bad, int(np.abs(w.astype(int).sum(1) - 128).max()))
    tests["weights_sum_128_max_dev"] = max_bad
    tests["palette_le_8"] = all(len(c[2]) <= MAX_PAL for c in chunks)
    tests["no_weight_outside_palette"] = all((c[6][:, len(c[2]):] == 0).all() for c in chunks)
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
    stats["ashc"] = write_ashc(out / f"{a.enemy}.ashc", sk, bank, chunks)
    pals = [len(c[2]) for c in chunks]
    stats["chunks"] = dict(count=len(chunks), palette_hist={str(k): pals.count(k) for k in sorted(set(pals))})
    stats["cook_seconds"] = round(time.time() - t0, 1)
    (out / f"{a.enemy}.json").write_text(json.dumps(stats, indent=1, default=str))
    print(json.dumps({k: v for k, v in stats.items() if k != "clips"}, indent=1, default=str))
    ok = (max_bad == 0 and tests["palette_le_8"] and tests["no_weight_outside_palette"]
          and tests["bind_pose_max_err_mm"] < 1.0)
    print("UNIT TESTS:", "PASS" if ok else "FAIL")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
