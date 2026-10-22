#!/usr/bin/env python3
"""Cook a demon of the Asylum for GE hardware skinning + its idle clip, same formats as tools/cook_knight.py.
c2232 = Asylum Demon (the boss, upper hall; NpcParam 223200), c2230 = Stray Demon (the pit under it).

    tools/.venv/bin/python tools/cook_demon.py --chr c2232 --out-dir ~/ash-assets/cooked

Outputs (never committed): <chr>.ashc, <chr>.ashanim, <chr>.json.
Skeleton: FLVER has 149 (c2230) / 151 (c2232) bones, ~35 nubs/dummies without weights; KEEP below = 31 bones
(same names in both): tail cut to every other
joint, toes/fingers/ears/tongue/wing digits collapse into feet/hands/head/wings, R_weapon (the hammer) into R Hand.
"""
from __future__ import annotations

import argparse
import json
import sys
import time
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).parent))
from ashm import BF_ALPHA, BF_NOCULL, Batch, simplify, weld  # noqa: E402
from cook_knight import (MAX_PAL, S, Skeleton, chunk_batch, cook_clip, load_hkx_skeleton, quantize_clip,  # noqa: E402
                         quantize_weights, reconstruct, unify_corners, write_ashanim, write_ashc, anim_id)
from cook_scene import TexBank  # noqa: E402
from dsrlib import DSR, Binder, binder_flver, chr_textures, mesh_arrays, mesh_textures, mesh_triangles, valid_meshes  # noqa: E402

CHR = "c2232"
KEEP = ["Pelvis", "tail_00", "tail_02", "tail_04", "tail_06",
        "Spine", "R Thigh", "R Calf", "R Foot", "RThighTwist", "L Thigh", "L Calf", "L Foot", "LThighTwist",
        "R_wing_00", "R_wing_01", "L_wing_00", "L_wing_01",
        "Spine1", "Neck", "Head", "Jaw",
        "L Clavicle", "L UpperArm", "L Forearm", "L Hand",
        "R Clavicle", "R UpperArm", "R Forearm", "R Hand", "R_weapon"]
CLIPS = [("idle", "a00_0000", True)]


def cook_mesh(sk: Skeleton, bank: TexBank, budget: int, stats: dict) -> list[Batch]:
    f = binder_flver(Binder.from_path(DSR / "chr" / f"{CHR}.chrbnd.dcx"))
    tx = chr_textures(CHR)
    tx.update(chr_textures("c2239"))  # the body texture lives in the shared c2239 pack (as tools/cook_scene.py)
    K = len(sk.keep)
    names = [b.name for b in f.bones]
    f2k = np.array([sk.to_kept[n] for n in names])
    parts, src = [], 0
    for m in valid_meshes(f):
        dstem = mesh_textures(m).get("g_Diffuse")
        if not dstem or dstem not in tx:
            print("  skip mesh without diffuse", dstem)
            continue
        d = mesh_arrays(m)
        tri = mesh_triangles(m)
        src += len(tri)
        pos = d["pos"].astype(np.float64)
        nrm = d.get("normal", np.tile([0, 1, 0], (len(pos), 1)))[:, :3].astype(np.float64)
        W = np.zeros((len(pos), K), np.float32)
        bi, bw = d["bone_indices"], d.get("bone_weights")
        if bw is None:
            bw = np.zeros(bi.shape, np.float32); bw[:, 0] = 1
        lut = np.array(m.bone_indices) if len(m.bone_indices) else None
        for kk in range(bi.shape[1]):
            g = lut[np.clip(bi[:, kk], 0, len(lut) - 1)] if lut is not None else bi[:, kk]
            np.add.at(W, (np.arange(len(pos)), f2k[np.clip(g, 0, len(f2k) - 1)]), bw[:, kk])
        W[W.sum(1) < 1e-6, 0] = 1
        pos[:, 0] *= -1; nrm[:, 0] *= -1
        ti = bank.add((CHR, dstem), lambda: tx[dstem][0], 128)
        flags = BF_ALPHA | BF_NOCULL if bank.items[ti].alpha else 0
        parts.append(Batch(ti, pos.astype(np.float32), d["uv0"].astype(np.float32), nrm.astype(np.float32), tri,
                           flags, f"{CHR}:{dstem}", W))
    total = sum(p.ntri for p in parts)
    ratio = min(1.0, budget / max(1, total))
    out = []
    for p in parts:
        s = unify_corners(simplify(weld(p), max(8, int(p.ntri * ratio))))
        s.col = (s.col / (np.linalg.norm(s.col, axis=1, keepdims=True) + 1e-9)).astype(np.float32)
        out.append(s)
    stats["mesh"] = dict(src_tris=src, tris=sum(b.ntri for b in out))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out-dir", required=True)
    ap.add_argument("--budget", type=int, default=3500)
    ap.add_argument("--chr", default="c2232", help="c2232 Asylum Demon (boss) or c2230 Stray Demon")
    args = ap.parse_args()
    global CHR
    CHR = args.chr
    t0 = time.time()
    out = Path(args.out_dir).expanduser()
    out.mkdir(parents=True, exist_ok=True)
    sk = Skeleton(KEEP, CHR)
    assert len(sk.keep) <= 32
    weighted = len(sk.names)
    stats: dict = dict(skeleton=dict(flver_bones=weighted, kept=len(sk.keep), keep=sk.keep))
    hsk = load_hkx_skeleton(CHR)
    stats["skeleton"]["hkx_bones"] = len(hsk.skeleton.bones)
    missing = [n for n in sk.keep if n not in {b.name for b in hsk.skeleton.bones}]
    stats["skeleton"]["kept_without_hkx_track"] = missing
    clips, tests = [], {}
    for name, a, loop in CLIPS:
        c = cook_clip(sk, hsk, a, [CHR])
        qc = quantize_clip(c)
        rec = reconstruct(sk, qc, c["frames"])
        ref = np.einsum("ij,fkjl,lm->fkim", S, c["arma"], S)
        err = float(np.abs(rec[:, :, :3, 3] - ref[:, :, :3, 3]).max())
        tests[f"{name}_max_joint_err_mm"] = round(err * 1000, 3)
        clips.append((name, anim_id(a), loop, c, qc))
        print(f"clip {name} {c['src']} frames={c['frames']} root_end={c['root'][-1].round(3).tolist()} "
              f"joint_err={err * 1000:.2f} mm", flush=True)
    stats["ashanim"] = write_ashanim(out / f"{CHR}.ashanim", len(sk.keep), clips)
    bank = TexBank(128)
    batches = cook_mesh(sk, bank, args.budget, stats)
    chunks, max_bad = [], 0
    for b in batches:
        Wq = quantize_weights(b.attr)
        for pal, used, tri, w in chunk_batch(b, Wq):
            chunks.append((b.tex, b.flags, pal, b.pos[used], b.uv[used], b.col[used], w, tri))
            max_bad = max(max_bad, int(np.abs(w.astype(int).sum(1) - 128).max()))
    tests["weights_sum_128_max_dev"] = max_bad
    tests["palette_le_8"] = all(len(c[2]) <= MAX_PAL for c in chunks)
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
    stats["ashc"] = write_ashc(out / f"{CHR}.ashc", sk, bank, chunks)
    stats["cook_seconds"] = round(time.time() - t0, 1)
    (out / f"{CHR}.json").write_text(json.dumps(stats, indent=1, default=str))
    print(json.dumps(stats, indent=1, default=str))
    ok = max_bad == 0 and tests["palette_le_8"] and worst < 1e-3
    print("UNIT TESTS:", "PASS" if ok else "FAIL")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
