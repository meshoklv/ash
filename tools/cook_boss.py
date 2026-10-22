#!/usr/bin/env python3
"""Cook the Asylum Demon boss (c2232) for the GE + its full move set + boss data (hitboxes, windows, stats).

    tools/.venv/bin/python tools/cook_boss.py --out-dir ~/ash-assets/cooked

Which model: in m18_01 the boss is **c2232** [проверено]: FMG NPC_name 2232 = «Демон Прибежища» / «Asylum Demon»,
MSB c2232_0000 entity 1810800, NpcParam 223200 («チュート ミニデブデーモン», HP 813), AI 223200, upper hall
(3.0, 198.06, -29.54). c2230 in the same map is the demon in the pit below (NpcParam 223000, y 182.11).

Outputs (never committed, FromSoftware-derived):
  boss.ashc     skinned mesh, <= 32 bones (31 of 151, same reduction as tools/cook_demon.py), chunks <= 8 bones
  boss.ashanim  clips (names below), translations with t_scale 1024 (+-32 m: the drop from the ceiling)
  boss.ashboss  boss data (format in write_ashboss), read by src/boss/boss.c
  boss_name.ashtx  the Russian name from FMG rendered for the HUD (write_name_tex)
  boss.json     stats + unit tests

Data chain per attack [формула ID по DSAnimStudio, совпадение строк проверено на DSR]:
  TAE c2232 event type 1/2 -> BehaviorJudgeID J -> BehaviorParam 200000000 + 22320*1000 + J -> AtkParam_Npc
  -> hit spheres / capsules between FLVER dummies (Hitbox{0..3}{Start,End}ModelPoint, Hitbox{0..3}Radius).
"""
from __future__ import annotations

import argparse
import dataclasses
import json
import logging
import struct
import sys
import time
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).parent))
logging.disable(logging.WARNING)
import cook_knight  # noqa: E402
from ashm import BF_ALPHA, BF_NOCULL, Batch, simplify, weld  # noqa: E402
from cook_knight import (MAX_PAL, S, Skeleton, chunk_batch, cook_clip, load_hkx_skeleton, quantize_clip,  # noqa: E402
                         quantize_weights, reconstruct, unify_corners, write_ashanim, write_ashc, anim_id)
from cook_scene import TexBank  # noqa: E402
from dsrlib import DSR, Binder, binder_flver, binder_textures, chr_textures, mesh_arrays, mesh_textures, mesh_triangles, valid_meshes  # noqa: E402
from tae_ds1 import read_tae  # noqa: E402

CHR = "c2232"
NPC_ID = 223200          # MSB c2232_0000 character_id
VARIATION = 22320        # NpcParam 223200 BehaviorVariationID
NAME_ID = 2232           # FMG NPC_name
KEEP = ["Pelvis", "tail_00", "tail_02", "tail_04", "tail_06",
        "Spine", "R Thigh", "R Calf", "R Foot", "RThighTwist", "L Thigh", "L Calf", "L Foot", "LThighTwist",
        "R_wing_00", "R_wing_01", "L_wing_00", "L_wing_01",
        "Spine1", "Neck", "Head", "Jaw",
        "L Clavicle", "L UpperArm", "L Forearm", "L Hand",
        "R Clavicle", "R UpperArm", "R Forearm", "R Hand", "R_weapon"]
# (runtime name, anim, loop). Meaning of non-attack clips from TAE + BehaviorParam names + root motion — [версия]
# where marked in docs/boss.md.
CLIPS = [
    ("idle", "a00_0000", True), ("walk", "a00_0200", True), ("walk_b", "a00_0201", True),
    ("walk_l", "a00_0202", True), ("walk_r", "a00_0203", True), ("run", "a00_0500", True),
    ("turn_l90", "a00_0602", False), ("turn_r90", "a00_0603", False),
    ("turn_l180", "a00_0604", False), ("turn_r180", "a00_0605", False),
    ("step_b", "a00_0701", False), ("fall", "a00_1500", True), ("land", "a00_1750", False),
    ("death", "a00_2200", False), ("dead", "a00_2250", True),
    ("stagger", "a00_2520", False), ("death_down", "a00_2521", False), ("dead_down", "a00_2522", True),
    *[(f"a{n}", f"a00_{n}", False) for n in (3000, 3001, 3002, 3003, 3004, 3005, 3006, 3007, 3008, 3009,
                                              3010, 3011, 3012, 3013, 3014)],
    ("a3020", "a00_3020", True), ("a3021", "a00_3021", True), ("a3022", "a00_3022", True),
    ("a3023", "a00_3023", False), ("a3500", "a00_3500", False), ("ev9000", "a00_9000", False),
    ("ev9060", "a00_9060", False),
]
DUMMY_IDS = (3, 4, 5, 7, 11, 12, 20, 21, 30)
T_SCALE = 1024.0


def P(gp, n):
    return next(v for k, v in gp.params.items() if k.endswith("\\" + n))


def cook_mesh(sk: Skeleton, bank: TexBank, budget: int, stats: dict, flv) -> list[Batch]:
    tx = binder_textures(Binder.from_path(DSR / "chr" / f"{CHR}.chrbnd.dcx"))
    tx.update(chr_textures("c2239"))  # body texture lives in the shared c2239 pack (hammer is in c2232.tpf)
    K = len(sk.keep)
    f2k = np.array([sk.to_kept[b.name] for b in flv.bones])
    parts, src = [], 0
    for m in valid_meshes(flv):
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


# --------------------------------------------------------------------------- boss data

def boss_data(sk: Skeleton, flv, tae: dict, clip_ids: dict[int, int], stats: dict):
    """-> (attacks, windows, dummies, info). Everything from DSR files of the user's copy."""
    from soulstruct.darksouls1r.params import GameParamBND
    from soulstruct.base.text.fmg import FMG
    gp = GameParamBND.from_path(DSR / "param/GameParam/GameParam.parambnd.dcx")
    npc = P(gp, "NpcParam").rows[NPC_ID]
    beh, atk = P(gp, "BehaviorParam").rows, P(gp, "AtkParam_Npc").rows

    # dummies: model-space position (mirrored x) + kept bone of the bone they follow
    names = [b.name for b in flv.bones]
    dmy = []
    for d in flv.dummies:
        if d.reference_id in DUMMY_IDS and d.reference_id not in [x[0] for x in dmy]:
            bone = d.attach_bone_index
            k = sk.to_kept[names[bone]] if 0 <= bone < len(names) else 0
            p = [float(v) for v in d.translate]
            dmy.append((d.reference_id, k, (-p[0], p[1], p[2])))
    dmy_index = {d[0]: i for i, d in enumerate(dmy)}

    attacks, atk_index = [], {}

    def atk_row(judge: int) -> int:
        bid = 200000000 + VARIATION * 1000 + judge
        if bid in atk_index:
            return atk_index[bid]
        b = beh[bid]
        if b.ReferenceType != 0 or b.ReferenceID not in atk:
            return -1
        a = atk[b.ReferenceID]
        hb = []
        for h in range(4):
            r = float(getattr(a, f"Hitbox{h}Radius"))
            s0 = getattr(a, f"Hitbox{h}StartModelPoint"); s1 = getattr(a, f"Hitbox{h}EndModelPoint")
            if r <= 0 or s0 <= 0 or s0 not in dmy_index:
                continue
            hb.append((dmy_index[s0], dmy_index.get(s1, -1) if s1 > 0 else -1, r))
        attacks.append(dict(behavior=bid, atk=b.ReferenceID, name=a.Name, hit=hb,
                            phys=a.PhysicalAttackPower, stam=a.StaminaAttackPower, poise=a.PoiseAttackPower,
                            knock=float(a.KnockbackDistance), impact=a.ImpactLevel,
                            flags=(1 if a.IgnoreGuard else 0) | (2 if "爆風" in a.Name else 0)))
        atk_index[bid] = len(attacks) - 1
        return atk_index[bid]

    # windows: kind 1 = hit (atk row), 2 = turn (deg/s), 3 = i-frames/invulnerable (flag 8) [версия],
    #          4 = combo/cancel point (flags 78/79/86, earliest start) [версия], 5 = super armor (flag 24/67) [версия]
    windows = []
    for aid, ci in clip_ids.items():
        a = tae.get(aid % 1000000 if aid < 1000000 else aid)
        if a is None:
            continue
        cancel = None
        for e in a.events:
            f0, f1 = round(e.start * 30), round(e.end * 30)
            if e.type in (1, 2):
                r = atk_row(e.ints[2])
                if r >= 0:
                    windows.append((ci, 1, f0, f1, r))
            elif e.type == 224:
                windows.append((ci, 2, f0, f1, int(round(e.floats[0]))))
            elif e.type == 0 and e.ints[0] == 8:
                windows.append((ci, 3, f0, f1, 0))
            elif e.type == 0 and e.ints[0] in (24, 67):
                windows.append((ci, 5, f0, f1, 0))
            elif e.type == 0 and e.ints[0] in (78, 79, 86):
                cancel = f0 if cancel is None else min(cancel, f0)
        if cancel is not None:
            windows.append((ci, 4, cancel, cancel, 0))

    fmg_name = ""
    for e in Binder.from_path(DSR / "msg/RUSSIAN/item.msgbnd.dcx").entries:
        if e.name.endswith("NPC_name_.fmg"):
            fmg_name = FMG.from_bytes(e.data).entries.get(NAME_ID, "")
    info = dict(hp=int(npc.MaximumHP), turn=float(npc.TurnVelocity), radius=float(npc.HitRadius),
                height=float(npc.HitHeight), weight=int(npc.Weight), souls=int(npc.SoulReward), name=fmg_name,
                defense=dict(phys=npc.PhysicalDefense, slash=npc.SlashDefense, strike=npc.StrikeDefense,
                             thrust=npc.ThrustDefense, magic=npc.MagicDefense, fire=npc.FireDefense))
    stats["boss"] = dict(info, attacks=[{k: v for k, v in x.items()} for x in attacks],
                         dummies=[dict(id=d[0], kept_bone=sk.keep[d[1]], pos=[round(v, 3) for v in d[2]]) for d in dmy],
                         windows=len(windows))
    return attacks, windows, dmy, info


REGIONS = (1812350, 1812351, 1812352, 1812990, 1812996, 1812301, 1812305, 1812300, 1812311, 1812320, 1812998,
           1812997, 1812400)


def boss_regions() -> list[tuple]:
    """MSB m18_01 regions the AI script / scripted moments read -> (id, shape 0 box / 1 sphere, pos, size).
    PSP space: x mirrored. Box: pos = bottom centre [версия], size = width (x), depth (z), height; sphere: size[0] = r.
    All of them have rotation 0 in the MSB except point regions (rotation unused) [проверено]."""
    from soulstruct.darksouls1r.maps import MSB
    msb = MSB.from_path(DSR / "map/MapStudio/m18_01_00_00.msb")
    out = []
    for r in msb.regions:
        if r.entity_id not in REGIONS:
            continue
        p = [float(v) for v in r.translate]
        sh = r.shape
        if hasattr(sh, "width"):
            out.append((r.entity_id, 0, (-p[0], p[1], p[2]), (float(sh.width), float(sh.depth), float(sh.height))))
        elif hasattr(sh, "radius"):
            out.append((r.entity_id, 1, (-p[0], p[1], p[2]), (float(sh.radius), 0.0, 0.0)))
    return out


def write_ashboss(path: Path, info, attacks, windows, dmy, regions) -> int:
    """.ashboss v1, little-endian.
    Header 96 B: "ASHB", u32 version=1, u32 n_atk, u32 n_win, u32 n_dmy, u32 hp, f32 turn_deg_s, f32 radius,
                 f32 height, u32 off_atk, u32 off_win, u32 off_dmy, u32 n_reg, u32 off_reg,
                 char name[40] (UTF-8, FMG NPC_name)
    Atk 48 B:   u16 phys, stam, poise, impact; f32 knock; u16 flags (1 ignore guard, 2 blast), u8 n_hit, pad;
                3 x { s8 dmy0, s8 dmy1 (-1 = sphere), u16 pad, f32 radius } ; u32 behavior id, u32 pad
    Win 8 B:    u8 clip, u8 kind (1 hit, 2 turn, 3 i-frames, 4 cancel point, 5 super armor), u8 f0, u8 f1 (frames
                at 30 Hz, clamped 255), s16 value (atk row / deg per s), u16 pad
    Dmy 16 B:   s16 id, s16 kept bone, f32 x, y, z (model space, mirrored like the mesh)
    Reg 32 B:   s32 id, u32 shape (0 box, 1 sphere), f32 pos[3], f32 size[3]"""
    hdr_len = 96
    atk_b = bytearray()
    for a in attacks:
        hits = a["hit"][:3] + [(-1, -1, 0.0)] * (3 - len(a["hit"][:3]))
        atk_b += struct.pack("<4HfHBx", a["phys"], a["stam"], a["poise"], a["impact"], a["knock"], a["flags"],
                             min(3, len(a["hit"])))
        for d0, d1, r in hits:
            atk_b += struct.pack("<bbxxf", d0, d1, r)
        atk_b += struct.pack("<II", a["behavior"], 0)
        assert len(atk_b) % 48 == 0
    win_b = b"".join(struct.pack("<BBBBhH", c, k, min(255, f0), min(255, f1), v, 0) for c, k, f0, f1, v in windows)
    dmy_b = b"".join(struct.pack("<hh3f", i, k, *p) for i, k, p in dmy)
    reg_b = b"".join(struct.pack("<iI3f3f", i, sh, *p, *sz) for i, sh, p, sz in regions)
    off_atk = hdr_len
    off_win = off_atk + len(atk_b)
    off_dmy = off_win + len(win_b)
    off_reg = off_dmy + len(dmy_b)
    name = info["name"].encode("utf-8")[:39]
    hdr = struct.pack("<4s5I3f5I40s", b"ASHB", 1, len(attacks), len(windows), len(dmy), info["hp"], info["turn"],
                      info["radius"], info["height"], off_atk, off_win, off_dmy, len(regions), off_reg, name)
    assert len(hdr) == hdr_len, len(hdr)
    data = hdr + bytes(atk_b) + win_b + dmy_b + reg_b
    path.write_bytes(data)
    return len(data)


def write_name_tex(path: Path, text: str) -> dict:
    """Boss name for the HUD (the PSP debug font has no Cyrillic): white text on transparent, PSM 4444, width padded
    to a power of two. File: u16 w (texture width), u16 h, u16 text_w, u16 pad[5] (16-byte header: the GE needs
    16-aligned texture addresses), then w*h u16 pixels (ABGR4444)."""
    from PIL import Image, ImageDraw, ImageFont
    font = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 12)
    l, t, r, b = font.getbbox(text)
    tw, th = r - l + 2, 16
    w = 1
    while w < tw:
        w *= 2
    im = Image.new("RGBA", (w, th), (0, 0, 0, 0))
    ImageDraw.Draw(im).text((1 - l, (th - (b - t)) // 2 - t), text, font=font, fill=(236, 228, 210, 255))
    a = np.asarray(im).astype(np.uint16) >> 4
    px = (a[..., 0] | (a[..., 1] << 4) | (a[..., 2] << 8) | (a[..., 3] << 12)).astype("<u2")
    path.write_bytes(struct.pack("<8H", w, th, tw, 0, 0, 0, 0, 0) + px.tobytes())
    return dict(w=w, h=th, text_w=tw)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out-dir", required=True)
    ap.add_argument("--budget", type=int, default=3500)
    ap.add_argument("--clips", default="", help="comma list of runtime clip names (default: all)")
    ap.add_argument("--skip-anim", action="store_true", help="keep boss.ashanim, recook mesh + boss data only")
    args = ap.parse_args()
    t0 = time.time()
    cook_knight.T_SCALE = T_SCALE  # quantize_clip / reconstruct / write_ashanim read the module global
    out = Path(args.out_dir).expanduser()
    out.mkdir(parents=True, exist_ok=True)
    sk = Skeleton(KEEP, CHR)
    assert len(sk.keep) <= 32
    flv = binder_flver(Binder.from_path(DSR / "chr" / f"{CHR}.chrbnd.dcx"))
    stats: dict = dict(chr=CHR, skeleton=dict(flver_bones=len(sk.names), kept=len(sk.keep), keep=sk.keep))
    hsk = load_hkx_skeleton(CHR)
    missing = [n for n in sk.keep if n not in {b.name for b in hsk.skeleton.bones}]
    stats["skeleton"]["kept_without_hkx_track"] = missing
    want = set(args.clips.split(",")) if args.clips else None
    clips, tests, clip_ids, cinfo = [], {}, {}, []
    for name, a, loop in CLIPS:
        if want and name not in want:
            continue
        if args.skip_anim:
            clip_ids[anim_id(a)] = len(clip_ids)
            continue
        c = cook_clip(sk, hsk, a, [CHR])
        qc = quantize_clip(c)
        rec = reconstruct(sk, qc, c["frames"])
        ref = np.einsum("ij,fkjl,lm->fkim", S, c["arma"], S)
        err = float(np.abs(rec[:, :, :3, 3] - ref[:, :, :3, 3]).max())
        tests[f"{name}_max_joint_err_mm"] = round(err * 1000, 3)
        clip_ids[anim_id(a)] = len(clips)
        clips.append((name, anim_id(a), loop, c, qc))
        r = c["root"]
        cinfo.append(dict(name=name, anim=a, frames=c["frames"], root_end=[round(float(v), 2) for v in r[-1]],
                          root_ymin=round(float(r[:, 1].min()), 2), root_ymax=round(float(r[:, 1].max()), 2)))
        print(f"clip {name:10s} {a} frames={c['frames']:3d} root_end={r[-1].round(2).tolist()} "
              f"y[{r[:, 1].min():.2f},{r[:, 1].max():.2f}] joint_err={err * 1000:.2f} mm", flush=True)
    if not args.skip_anim:
        stats["clips"] = cinfo
        stats["ashanim"] = write_ashanim(out / "boss.ashanim", len(sk.keep), clips)

    tae = read_tae(next(e.data for e in Binder.from_path(DSR / "chr" / f"{CHR}.anibnd.dcx").entries
                        if e.name.lower().endswith(".tae")))
    # TAE ids are plain (3000); anim_id("a00_3000") = 3000 too
    attacks, windows, dmy, info = boss_data(sk, flv, tae, clip_ids, stats)
    regions = boss_regions()
    stats["regions"] = [dict(id=i, shape=sh, pos=[round(v, 2) for v in p], size=[round(v, 2) for v in z])
                        for i, sh, p, z in regions]
    stats["ashboss_bytes"] = write_ashboss(out / "boss.ashboss", info, attacks, windows, dmy, regions)
    stats["name_tex"] = write_name_tex(out / "boss_name.ashtx", info["name"])

    bank = TexBank(128)
    batches = cook_mesh(sk, bank, args.budget, stats, flv)
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
    tests["hit_rows_with_volumes"] = sum(1 for a in attacks if a["hit"])
    tests["attacks"] = len(attacks)
    stats["tests"] = tests
    stats["ashc"] = write_ashc(out / "boss.ashc", sk, bank, chunks)
    stats["cook_seconds"] = round(time.time() - t0, 1)
    (out / "boss.json").write_text(json.dumps(stats, indent=1, default=str, ensure_ascii=False))
    print(json.dumps({k: v for k, v in stats.items() if k not in ("clips",)}, indent=1, default=str,
                     ensure_ascii=False)[:6000])
    worst_joint = max([v for k, v in tests.items() if k.endswith("_joint_err_mm")] or [0])
    ok = max_bad == 0 and tests["palette_le_8"] and worst < 1e-3 and worst_joint < 5 and attacks
    print("UNIT TESTS:", "PASS" if ok else "FAIL", f"(worst joint err {worst_joint} mm)")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
