#!/usr/bin/env python3
"""Cook the combat data of slice S1 (Undead Asylum, m18_01) into one table file for src/combat.

    tools/.venv/bin/python tools/cook_combat.py --out-dir ~/ash-assets/cooked [--no-clips]

Outputs (never committed - FromSoftware numbers):
  combat.ashcb          tables read by src/combat/cbdata.c (layout: src/combat/cbdata.h, version 1)
  combat.json           human-readable dump of what went in + sanity numbers (used by docs/combat.md)
  knight_combat.ashanim extra knight clips for combat (guard, hits, death, parry, riposte, ...), same skeleton and
                        format as knight.ashanim from tools/cook_knight.py (that cooker is not modified)

Sources (all from the DSR copy, $ASH_DSR):
  TAE      chr/c0000.anibnd (a00/a23/a48.tae), chr/c2500.anibnd, chr/c2550.anibnd, chr/c2230.anibnd - tools/tae.py
  PARAM    param/GameParam/GameParam.parambnd: AtkParam_Pc/Npc, BehaviorParam(_PC), EquipParamWeapon/Protector,
           ReinforceParamWeapon, CalcCorrectGraph, NpcParam, ThrowParam, CharaInitParam, Bullet, SpEffectParam
  dummies  FLVER dummy polys of chr/cXXXX.chrbnd and parts/WP_A_0201 (Longsword), WP_A_1516 (Knight Shield)

Behaviour id formulas are DSAnimStudio's (combat-ai notes 4.1): player 100000000 + V*1000 + J, NPC 200000000 + V*1000 + J,
V = EquipParamWeapon/NpcParam.BehaviorVariationID. Every resolved/unresolved id is counted in combat.json.
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
import tae  # noqa: E402
from dsrlib import DSR, Binder, binder_flver  # noqa: E402

VERSION = 1
MAP_MSB = "m18_01_00_00"

# ---------------------------------------------------------------- what the slice uses
PLAYER_CLASS = 3001          # CharaInitParam 初期キャラ_騎士 (Knight): stats only, see below
WEAPON_R = 201000            # Longsword (the model the knight cooker draws: WP_A_0201)
WEAPON_L = 1451000           # Knight Shield (model WP_A_1516)
ARMOR = (350000, 351000, 352000, 353000)   # Elite Knight set = model 9500 the knight cooker draws (HD/BD/AM/LG_A_9500)
# Kinds = character models. NPC ids actually placed in m18_01 are read from the MSB.
KINDS = ["c0000", "c2500", "c2550", "c2230", "c2232"]
# c2232 = the Asylum Demon of the boss fight (NpcParam 223200, AI 223200_battle); c2230 = Stray Demon in the pit
PLAYER_A00 = [(0, 6), (100, 165), (680, 744), (1600, 1611), (2000, 2102), (6000, 6085), (7000, 7001)]

# ChrActionFlag (TAE ev.0 FlagType, DSAnimStudio DS1 template) -> our state bit (src/combat/combat.h CB_ST_*)
FLAG_BIT = {8: 0, 94: 0,            # INVINCIBLE
            67: 1, 68: 1,           # INV_EXC_THROW
            24: 2,                  # SUPER_ARMOR
            45: 3,                  # NO_SUPER_ARMOR
            5: 4, 73: 4,            # PARRYABLE (attacker can be parried)
            3: 5,                   # GUARD
            7: 6,                   # NO_TURN
            4: 7,                   # CANCEL_RH
            16: 8,                  # CANCEL_LH
            11: 9,                  # CANCEL_MOVE
            26: 10,                 # CANCEL_ROLL
            32: 11,                 # CANCEL_ANY
            17: 12,                 # CANCEL_ON_HIT
            18: 13,                 # CANCEL_PARRY_OK
            1: 14, 35: 14,          # INPUT_RH
            9: 15,                  # INPUT_LH
            25: 16,                 # INPUT_ROLL
            12: 17, 40: 17, 47: 17, 48: 17,   # DEATH
            19: 18, 39: 18, 50: 18,           # NO_COLLISION
            22: 19,                 # CANCEL_GUARD
            49: 20, 55: 20,         # NO_LOCKON
            63: 21,                 # PARRY_TIMING
            71: 22,                 # SA_BREAK_UNRECOVERABLE
            87: 23,                 # COMMON_INPUT
            }
ONESHOT = {224: "f", 225: "B", 226: "B", 236: "f", 66: "i", 67: "i"}

# Reaction anims per kind (index = CB_R_* in combat.h). 0 = none. Player ids are XX*10000+YYYY (a00_2000 -> 2000).
#   DEATH, HIT_S, HIT_M, HIT_L, KNOCKDOWN, PARRIED, GUARD_HIT, GUARD_HIT_L, GUARD_BREAK, ROLL, BACKSTEP, PARRY
# Sources: death = first anim with flag 12 (Death State); parried = anim adding SpEffect 30
# 「パリィ崩され状態（投げ条件）」 (hollow 2100 [проверено]); hit/guard ids by TAE layout [версия, see docs/combat.md].
#   ... REPELLED (index 12, CB_R_REPELLED): the attacker's weapon bounced off a shield. a00_2050 [версия: chosen by the
#   pose - stick-figure check 15.10: weapon arm thrown up and back, body leans back, 56-69 frames, no hit windows;
#   4 variants 2050-2053; no name for it in the data].
REACT = {
    "c0000": [6000, 2000, 2004, 2010, 2014, 2100, 100, 130, 160, 710, 690, 485100, 2050],
    "c2500": [2200, 2000, 2004, 2010, 2014, 2100, 0, 0, 0, 0, 701, 0, 2050],
    "c2550": [2200, 2000, 2004, 2010, 2014, 2100, 0, 0, 0, 0, 701, 0, 2050],
    "c2230": [2200, 0, 0, 0, 0, 0, 0, 0, 0, 0, 701, 0, 0],
    "c2232": [2200, 0, 0, 0, 0, 0, 0, 0, 0, 0, 701, 0, 0],
}
# Hurt capsules (bone_a, bone_b, radius m) [придумал: by eye from the bind pose; DS1 uses Havok hit shapes we do not read]
HURT_BASE = [("Pelvis", "Spine1", 0.22), ("Spine1", "Neck", 0.21), ("Neck", "Head", 0.13),
             ("L_Thigh", "L_Calf", 0.11), ("L_Calf", "L_Foot", 0.08), ("R_Thigh", "R_Calf", 0.11),
             ("R_Calf", "R_Foot", 0.08), ("L_UpperArm", "L_Forearm", 0.08), ("L_Forearm", "L_Hand", 0.07),
             ("R_UpperArm", "R_Forearm", 0.08), ("R_Forearm", "R_Hand", 0.07)]
HURT_SCALE = {"c0000": 1.0, "c2500": 0.95, "c2550": 1.15, "c2230": 4.0, "c2232": 4.0}
# Player numbers not found in PARAM [версия, вики]: stamina from END (DS1 table: END 10 -> 91), regen 45/s,
# regen in guard -80%; roll/backstep costs not in BehaviorParam_PC (their PCBehavior 504 has StaminaCost 0).
PLAYER_STAMINA_BY_END = {8: 89, 9: 90, 10: 91, 11: 93, 12: 95}
PLAYER_HP_BY_VIT = {14: 572}          # [версия] Knight starting HP from the wiki
STAMINA_REGEN = 45.0
GUARD_REGEN_MULT = 0.2
ROLL_STAMINA = 8.0                   # [версия] not found in data
BACKSTEP_STAMINA = 8.0               # [версия]

T_S = 30.0


def load_params():
    from soulstruct.darksouls1r.params.gameparambnd import GameParamBND
    g = GameParamBND.from_path(DSR / "param/GameParam/GameParam.parambnd.dcx")
    return {k.rsplit("\\", 1)[1]: v.rows for k, v in g.params.items()}


def correct(graph, stat: float) -> float:
    """CalcCorrectGraph row -> percent (0..100) for a stat value. Same stage/exponent shape as DS3's, read from data."""
    xs = [graph.StageMaxIntercept0, graph.StageMaxIntercept1, graph.StageMaxIntercept2, graph.StageMaxIntercept3,
          graph.StageMaxIntercept4]
    ys = [getattr(graph, f"StageMaxSlope{i}", 0.0) for i in range(5)]
    es = [getattr(graph, f"AdjustmentMaxSlope{i}", 1.0) for i in range(5)]
    if stat <= xs[0]:
        return ys[0]
    for i in range(4):
        if stat <= xs[i + 1]:
            r = (stat - xs[i]) / max(1e-6, xs[i + 1] - xs[i])
            e = es[i]
            r = r ** e if e > 0 else 1 - (1 - r) ** (-e)
            return ys[i] + (ys[i + 1] - ys[i]) * r
    return ys[4]


def flver_dummies(binder_path: Path):
    f = binder_flver(Binder.from_path(binder_path))
    bones = f.bones
    arma = [np.array(list(t)) for t, r, s in f.get_bone_tree().get_bone_armature_space_transforms()]
    rots = [np.array(r.data) for t, r, s in f.get_bone_tree().get_bone_armature_space_transforms()]
    out = []
    for d in f.dummies:
        p = np.array(list(d.translate), np.float64)       # model (bind) space [проверено: toe/heel dummies]
        ab = d.attach_bone_index
        if ab >= 0:
            loc = rots[ab].T @ (p - arma[ab])               # bone-local
            bone = bones[ab].name
        else:
            loc, bone = p, ""
        out.append((d.reference_id, bone, loc, p))
    names = [b.name for b in bones]
    bind = {b.name: (arma[i], rots[i]) for i, b in enumerate(bones)}
    return out, names, bind


S = np.array([-1.0, 1.0, 1.0])   # mirror x (tools/cook_knight.py: FromSoft data is left-handed)


def mirror_local(loc, R):
    """Bone-local offset in FromSoft space -> bone-local offset in our mirrored space.
    Mirrored bone matrix is S M S, so a local point l maps to S l."""
    return loc * S


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out-dir", required=True)
    ap.add_argument("--no-clips", action="store_true", help="skip knight_combat.ashanim")
    args = ap.parse_args()
    t0 = time.time()
    out_dir = Path(args.out_dir).expanduser()
    out_dir.mkdir(parents=True, exist_ok=True)
    P = load_params()
    W, AP, AN, BP, BN = P["EquipParamWeapon"], P["AtkParam_Pc"], P["AtkParam_Npc"], P["BehaviorParam_PC"], P["BehaviorParam"]
    NP, TP, CI, PR, CG, RW = P["NpcParam"], P["ThrowParam"], P["CharaInitParam"], P["EquipParamProtector"], \
        P["CalcCorrectGraph"], P["ReinforceParamWeapon"]
    BUL = P["Bullet"]
    rep: dict = dict(unresolved_behaviors=[], notes=[])

    # ------------------------------------------------ NPCs placed in the map
    from soulstruct.darksouls1r.maps import MSB
    msb = MSB.from_path(DSR / "map/MapStudio" / f"{MAP_MSB}.msb")
    npc_ids = sorted({c.character_id for c in msb.characters if c.model.name in KINDS[1:]})
    rep["msb_npcs"] = sorted({(c.model.name, c.character_id) for c in msb.characters if c.model.name in KINDS[1:]})
    var_of_kind = {}
    for i in npc_ids:
        var_of_kind.setdefault("c%04d" % (NP[i].BehaviorVariationID // 10), NP[i].BehaviorVariationID)

    # ------------------------------------------------ attack rows (deduplicated by (table, id))
    atk_rows: list[bytes] = []
    atk_index: dict = {}
    atk_dump = {}

    def add_atk(table: str, aid: int):
        key = (table, aid)
        if key in atk_index:
            return atk_index[key]
        r = (AP if table == "pc" else AN).get(aid)
        if r is None:
            return 0xFFFF
        radius = [r.Hitbox0Radius, r.Hitbox1Radius, r.Hitbox2Radius, r.Hitbox3Radius]
        da = [r.Hitbox0StartModelPoint, r.Hitbox1StartModelPoint, r.Hitbox2StartModelPoint, r.Hitbox3StartModelPoint]
        db = [r.Hitbox0EndModelPoint, r.Hitbox1EndModelPoint, r.Hitbox2EndModelPoint, r.Hitbox3EndModelPoint]
        ht = [r.Hitbox0HitType, r.Hitbox1HitType, r.Hitbox2HitType, r.Hitbox3HitType]
        flags = (1 if r.IgnoreGuard else 0) | (2 if r.NoStaminaDamage else 0) | (4 if r.IgnoreInvincibilityFrames else 0) \
            | (8 if table == "npc" else 0)
        rec = struct.pack("<4f4h4h4B8H8H2fH6BI",
                          *radius, *[int(x) for x in da], *[int(x) for x in db], *[int(x) & 255 for x in ht],
                          r.PhysicalAttackPowerPercentage, r.MagicAttackPowerPercentage, r.FireAttackPowerPercentage,
                          r.LightningAttackPowerPercentage, r.StaminaAttackPowerPercentage, r.GuardAttackPercentage,
                          r.GuardBreakPercentage, r.PoiseAttackPercentage,
                          r.PhysicalAttackPower, r.MagicAttackPower, r.FireAttackPower, r.LightningAttackPower,
                          r.StaminaAttackPower, r.PoiseAttackPower, r.GuardAttackPower & 0xFFFF,
                          r.GuardBreakPower & 0xFFFF,
                          r.KnockbackDistance, r.HitStopTime, max(0, r.ThrowID) & 0xFFFF,
                          r.ImpactLevel & 255, r.AttackAttribute & 255, r.ElementAttribute & 255,
                          r.ModelPointSource & 255, r.ThrowFlag & 255, flags, aid)
        assert len(rec) == CB_ATK_SIZE, len(rec)
        atk_index[key] = len(atk_rows)
        atk_rows.append(rec)
        atk_dump[f"{table}:{aid}"] = dict(radius=[round(x, 3) for x in radius], dmy_a=da, dmy_b=db,
                                         pct_phys=r.PhysicalAttackPowerPercentage, atk_phys=r.PhysicalAttackPower,
                                         pct_sa=r.PoiseAttackPercentage, atk_sa=r.PoiseAttackPower,
                                         atk_stam=r.StaminaAttackPower, knockback=round(r.KnockbackDistance, 3),
                                         hitstop=round(r.HitStopTime, 3), dmg_level=r.ImpactLevel,
                                         src=r.ModelPointSource, throw_flag=r.ThrowFlag)
        return atk_index[key]

    def behavior(pc: bool, var: int, judge: int, common: bool):
        """-> (BehaviorParam row or None, id)"""
        tab = BP if pc else BN
        if common:
            return tab.get(judge), judge
        base = 100000000 if pc else 200000000
        for v in (var, var // 100 * 100):
            bid = base + v * 1000 + judge
            if bid in tab:
                return tab[bid], bid
        return None, base + var * 1000 + judge

    # ------------------------------------------------ anims
    kinds_rec, anims, states, hits, evts = [], [], [], [], []
    anim_dump = {}
    w_r, w_l = W[WEAPON_R], W[WEAPON_L]
    for kind in KINDS:
        pc = kind == "c0000"
        b = Binder.from_path(DSR / "chr" / f"{kind}.anibnd.dcx")
        taes = {}
        for e in b.entries:
            nm = e.name.rsplit("\\", 1)[1].lower()
            if nm.endswith(".tae"):
                taes[nm[:-4]] = e.data
        sel = []
        if pc:
            for cat, prefix_ok in (("a00", lambda i: any(lo <= i < hi for lo, hi in PLAYER_A00)),
                                   ("a23", lambda i: True), ("a48", lambda i: True)):
                v, tid, o = tae.parse(taes[cat])
                for aid in sorted(o):
                    if prefix_ok(aid):
                        sel.append((int(cat[1:]) * 10000 + aid, o[aid], int(cat[1:])))
        else:
            data = next(iter(taes.values()))
            v, tid, o = tae.parse(data)
            sel = [(aid, o[aid], None) for aid in sorted(o)]
        anim_first = len(anims)
        for full_id, evs, cat in sel:
            st0, h0, e0 = len(states), len(hits), len(evts)
            length = 0
            cost = 0
            my_hits = []
            for ev in evs:
                f0, f1 = ev.frames()
                length = max(length, f1)
                if ev.type == 0:
                    flag, = ev.unpack("i")
                    if flag in FLAG_BIT:
                        states.append(struct.pack("<2H2BH", f0, f1, FLAG_BIT[flag], flag, 0))
                elif ev.type in (1, 5, 304, 2):
                    if ev.type == 1:
                        atype, idx, judge = ev.unpack("3i")
                    elif ev.type == 5:
                        idx, judge = ev.unpack("2i"); atype = 0
                    elif ev.type == 304:
                        _, idx, judge = ev.unpack("Hhi"); atype = 0
                    else:
                        dmy, _, judge = ev.unpack("3i"); atype = 0; idx = dmy
                    if pc:
                        var = w_l.BehaviorVariationID if cat == 48 else w_r.BehaviorVariationID
                    else:
                        var = var_of_kind.get(kind, 0)
                    row, bid = behavior(pc, var, judge, ev.type == 5)
                    atk = 0xFFFF
                    stam = 0
                    bullet_r = 0
                    if row is None:
                        rep["unresolved_behaviors"].append(dict(kind=kind, anim=full_id, ev=ev.type, id=bid))
                    else:
                        stam = row.StaminaCost
                        if row.ReferenceType == 0:
                            atk = add_atk("pc" if pc else "npc", row.ReferenceID)
                        elif row.ReferenceType == 1 and row.ReferenceID in BUL:
                            # bullet: its AtkParam (or the one of its BulletOnHit explosion) as a sphere at the
                            # event's dummy poly, no flight [версия]
                            bu = BUL[row.ReferenceID]
                            aid = bu.BulletAttack
                            brad = max(bu.InitialHitRadius, bu.FinalHitRadius)
                            if aid <= 0 and bu.BulletOnHit > 0 and bu.BulletOnHit in BUL:
                                b2 = BUL[bu.BulletOnHit]
                                aid = b2.BulletAttack
                                brad = max(b2.InitialHitRadius, b2.FinalHitRadius)
                                if b2.RadiusIncreaseTime > 0:   # expanding shock wave: keep it alive while it grows
                                    f1 = max(f1, f0 + int(round(b2.RadiusIncreaseTime * T_S)))
                            atk = add_atk("pc" if pc else "npc", aid) if aid > 0 else 0xFFFF
                            bullet_r = int(round(brad * 100))
                        else:
                            rep["notes"].append(f"{kind} {full_id}: behavior {bid} refType {row.ReferenceType}")
                    kind_code = {1: 1, 5: 5, 304: 4, 2: 2}[ev.type]
                    hits.append(struct.pack("<3H2B2HhH", f0, f1, atk, kind_code, atype & 255, judge & 0xFFFF,
                                            stam, idx if ev.type == 2 else -1, bullet_r))
                    my_hits.append(dict(f=[f0, f1], ev=ev.type, type=atype, judge=judge, beh=bid, atk=atk, stam=stam,
                                        bullet_r_cm=bullet_r))
                    if ev.type in (1, 304):
                        cost = max(cost, stam)
                elif ev.type in ONESHOT:
                    fmt = ONESHOT[ev.type]
                    val = ev.unpack(fmt)[0]
                    evts.append(struct.pack("<3Hxxf", f0, f1, ev.type, float(val)))
            anims.append(struct.pack("<I8H", full_id, KINDS.index(kind), length, st0, len(states) - st0, h0,
                                     len(hits) - h0, e0, len(evts) - e0) + struct.pack("<2H", cost, 0))
            if my_hits:
                anim_dump[f"{kind}:{full_id}"] = dict(len=length, cost=cost, hits=my_hits)
        n_anim = len(anims) - anim_first
        kinds_rec.append((kind, anim_first, n_anim))
        print(f"{kind}: {n_anim} anims", flush=True)

    # ------------------------------------------------ dummies + hurt capsules per kind
    dummies, hurts, kinds_bin = [], [], []
    wep_dmy = {}
    for src, part in ((1, "WP_A_0201"), (2, "WP_A_1516")):
        ds, _, _ = flver_dummies(DSR / "parts" / f"{part}.partsbnd.dcx")
        wep_dmy[src] = [(i, "R_Weapon" if src == 1 else "L_Weapon", loc, loc) for i, b_, loc, p in ds]
    for ki, (kind, a0, na) in enumerate(kinds_rec):
        ds, names, bind = flver_dummies(DSR / "chr" / f"{kind}.chrbnd.dcx")
        d0 = len(dummies)
        seen = set()
        lists = [(0, ds)] + ([(1, wep_dmy[1]), (2, wep_dmy[2])] if kind == "c0000" else [])
        for src, lst in lists:
            for rid, bone, loc, model in lst:
                if (src, rid) in seen or rid in (15, 200) or rid < 0:
                    continue          # 15/200 are per-bone helper points, not hit points
                seen.add((src, rid))
                if src == 0:
                    lm = loc * S
                    mm = np.asarray(model) * S
                else:
                    # weapon space == R/L_Weapon bone space (cook_knight attaches rigid weapons to these bones)
                    lm = np.asarray(loc) * S
                    wb = bind["R_Weapon" if src == 1 else "L_Weapon"]
                    mm = (wb[1] @ np.asarray(loc) + wb[0]) * S
                dummies.append(struct.pack("<hBx3f3f20s", rid, src, *lm, *mm, bone.encode()[:19]))
        nd = len(dummies) - d0
        h0 = len(hurts)
        sc = HURT_SCALE[kind]

        def find(n):
            for c in (n, n.replace("_", " ")):
                if c in bind:
                    return c
            return None
        for a, bnm, r in HURT_BASE:
            fa, fb = find(a), find(bnm)
            if not fa or not fb:
                continue
            pa = bind[fa][0] * S
            pb = bind[fb][0] * S
            hurts.append(struct.pack("<20s20sf3f3f", fa.encode(), fb.encode(), r * sc, *pa, *pb))
        nh = len(hurts) - h0
        react = REACT[kind] + [0] * (16 - len(REACT[kind]))
        kinds_bin.append(struct.pack("<8s6H", kind.encode(), a0, na, d0, nd, h0, nh) + struct.pack("<16I", *react))
        print(f"{kind}: {nd} dummies, {nh} hurt capsules", flush=True)

    # ------------------------------------------------ NPC rows
    npcs = []
    npc_dump = {}
    SPE = P["SpEffectParam"]
    for i in npc_ids:
        r = NP[i]
        # Level SpEffects in NpcParam.spEffectID0..7 ("レベル14": HP x2.5, attack x2.4, defense x3 ...) are permanent
        # [версия, consistent with the wiki: Stray Demon 2100 x 2.5 = 5250 HP]
        m = dict(hp=1.0, st=1.0, atk=1.0, df=1.0, sd=1.0)
        lv = 0
        for k in range(8):
            sid = getattr(r, f"SpecialEffectID{k}", 0)
            se = SPE.get(sid) if sid > 0 else None
            if se is None:
                continue
            if se.MaxHPMultiplier not in (0.0, 1.0):
                lv = sid
            m["hp"] *= se.MaxHPMultiplier or 1.0
            m["st"] *= se.MaxStaminaMultiplier or 1.0
            m["atk"] *= se.PhysicalAttackPowerMultiplier or 1.0
            m["df"] *= se.PhysicalDefenseMultiplier or 1.0
            m["sd"] *= se.OutgoingStaminaDamageMultiplier or 1.0
        hp = int(round(r.MaximumHP * m["hp"]))
        st = int(round(r.MaxStamina * m["st"]))
        dd = [int(round(x * m["df"])) for x in (r.PhysicalDefense, r.MagicDefense, r.FireDefense, r.LightningDefense)]
        npcs.append(struct.pack("<2I3f4H7h2H2x3fI", i, r.BehaviorVariationID, r.HitRadius, r.HitHeight, r.TurnVelocity,
                                hp, st, r.StaminaRecoveryBaseSpeed, r.MaxPoise, *dd,
                                r.SlashDefense, r.StrikeDefense, r.ThrustDefense, r.SoulReward & 0xFFFF,
                                r.Weight & 0xFFFF, m["atk"], m["sd"], m["df"], lv))
        npc_dump[i] = dict(name=r.Name, hp_param=r.MaximumHP, hp=hp, stamina=st, regen=r.StaminaRecoveryBaseSpeed,
                           poise=r.MaxPoise, def_phys_param=r.PhysicalDefense, defs=dd, souls=r.SoulReward,
                           var=r.BehaviorVariationID, level_speffect=lv, mult={k: round(v, 3) for k, v in m.items()},
                           hit_radius=round(r.HitRadius, 3), hit_height=round(r.HitHeight, 3))

    # ------------------------------------------------ player
    ci = CI[PLAYER_CLASS]
    stats = dict(level=ci.Level, vit=ci.Vitality, att=ci.Attunement, end=ci.Endurance, str=ci.Strength,
                 dex=ci.Dexterity, int=ci.Intelligence, fai=ci.Faith, luck=ci.Luck, res=ci.Resistance)

    def weapon_rec(wid):
        r = W[wid]
        g = CG[getattr(r, "ScalingFormulaType", 0) if getattr(r, "ScalingFormulaType", 0) in CG else 0]
        rf = RW.get(r.WeaponUpgradeID, RW[0])
        cs, cd = correct(g, stats["str"]), correct(g, stats["dex"])
        # AR = base * reinforce + base * scaling% * correct% (DS1 physical; magic/fire/lightning scale with INT/FTH,
        # not used by these weapons). [версия: community AR formula]
        base = r.BasePhysicalDamage * rf.PhysicalDamageMultiplier
        ar = base + r.BasePhysicalDamage * (r.StrengthScaling * rf.StrengthScalingMultiplier / 100.0 * cs / 100.0 +
                                            r.DexterityScaling * rf.DexterityScalingMultiplier / 100.0 * cd / 100.0)
        rec = struct.pack("<IH2B6H8fH2B2Hf", wid, r.BehaviorVariationID, r.AttackAnimationCategory, r.GuardLevel,
                          r.BasePhysicalDamage, r.BaseMagicDamage, r.BaseFireDamage, r.BaseLightningDamage,
                          r.BaseStaminaDamage, r.BasePoiseDamage,
                          ar, r.BaseMagicDamage, r.BaseFireDamage, r.BaseLightningDamage,
                          r.PhysicalGuardPercentage, r.MagicGuardPercentage, r.FireGuardPercentage,
                          r.LightningGuardPercentage,
                          r.GuardStaminaDefense, r.MaxParryWindowDuration & 255,
                          (1 if r.ParryEnabled else 0) | (2 if r.GuardEnabled else 0),
                          r.DeflectOnBlock & 0xFFFF, r.DeflectOnAttack & 0xFFFF, r.Weight)
        return rec, dict(id=wid, name=r.Name, var=r.BehaviorVariationID, cat=r.AttackAnimationCategory,
                         base_phys=r.BasePhysicalDamage, ar_phys=round(ar, 2), str_corr=round(cs, 2),
                         dex_corr=round(cd, 2), stam_dmg=r.BaseStaminaDamage, poise_dmg=r.BasePoiseDamage,
                         guard=[r.PhysicalGuardPercentage, r.MagicGuardPercentage, r.FireGuardPercentage,
                                r.LightningGuardPercentage], stability=r.GuardStaminaDefense,
                         parry_window=r.MaxParryWindowDuration, deflect_block=r.DeflectOnBlock,
                         deflect_attack=r.DeflectOnAttack, weight=r.Weight)
    weapons, wdump = [], []
    for wid in (WEAPON_R, WEAPON_L):
        rec, dd = weapon_rec(wid)
        weapons.append(rec); wdump.append(dd)
    arm = [PR[a] for a in ARMOR]
    poise = sum(a.Poise for a in arm)
    dphys, dmag, dfire, dthun = (sum(getattr(a, f) for a in arm) for f in
                                 ("PhysicalDefense", "MagicDefense", "FireDefense", "LightningDefense"))
    hp = PLAYER_HP_BY_VIT.get(stats["vit"], 400)
    sp = PLAYER_STAMINA_BY_END.get(stats["end"], 90)
    player = struct.pack("<10H2Hf2I5H2xfff4x", stats["level"], stats["vit"], stats["att"], stats["end"], stats["str"],
                         stats["dex"], stats["int"], stats["fai"], stats["luck"], stats["res"], hp, sp, STAMINA_REGEN,
                         WEAPON_R, WEAPON_L, poise, dphys, dmag, dfire, dthun, ROLL_STAMINA, BACKSTEP_STAMINA,
                         GUARD_REGEN_MULT)
    pdump = dict(stats=stats, hp=hp, stamina=sp, stamina_regen=STAMINA_REGEN, poise=poise,
                 defense=[dphys, dmag, dfire, dthun], armor=[(a, PR[a].Name) for a in ARMOR],
                 roll_stamina=ROLL_STAMINA, backstep_stamina=BACKSTEP_STAMINA)

    # ------------------------------------------------ throws (player -> our kinds)
    throws, tdump = [], []
    defs = {int(k[1:]) for k in KINDS}
    for i, r in sorted(TP.items()):
        if r.AttackingCharacterModel == 0 and r.DefendingCharacterModel in defs and r.AttackEnabled:
            throws.append(struct.pack("<I2H6f2I2h2BH", i, r.AttackingCharacterModel, r.DefendingCharacterModel,
                                      r.MaxDistance, r.MinDifferenceInFacingAngle, r.MaxDifferenceInFacingAngle,
                                      r.MaxAngleToDefender, r.MaxDistanceAbove, r.MaxDistanceBelow,
                                      r.AttackerAnimation, r.DefenderAnimation, r.SnapToAttackerModelPoint,
                                      r.SnapToDefenderModelPoint, r.ThrowType & 255, 1 if r.AttackerTurns else 0, 0))
            tdump.append(dict(id=i, name=r.Name, defender=r.DefendingCharacterModel, dist=r.MaxDistance,
                              ang=[r.MinDifferenceInFacingAngle, r.MaxDifferenceInFacingAngle], to_def=r.MaxAngleToDefender,
                              atk_anim=r.AttackerAnimation, def_anim=r.DefenderAnimation, type=r.ThrowType))

    # ------------------------------------------------ write
    secs = [kinds_bin, anims, states, hits, evts, atk_rows, dummies, hurts, npcs, weapons, throws, [player]]
    sizes = [CB_KIND_SIZE, 24, 8, 16, 12, CB_ATK_SIZE, 48, 68, 64, 64, 48, 64]
    hdr_size = 16 + 8 * len(secs)
    body = bytearray(b"\0" * hdr_size)
    table = []
    for recs, sz in zip(secs, sizes):
        while len(body) % 16:
            body.append(0)
        for r_ in recs:
            assert len(r_) == sz, (sz, len(r_))
        table.append((len(body), len(recs)))
        body += b"".join(recs)
    hdr = struct.pack("<4s3I", b"ACMB", VERSION, len(secs), 0) + b"".join(struct.pack("<2I", o, n) for o, n in table)
    body[:hdr_size] = hdr
    (out_dir / "combat.ashcb").write_bytes(body)

    # sanity numbers for the docs / unit tests
    rep.update(version=VERSION, bytes=len(body), counts=dict(zip(
        ["kinds", "anims", "states", "hits", "evts", "atk", "dummies", "hurts", "npcs", "weapons", "throws", "player"],
        [len(s) for s in secs])), player=pdump, weapons=wdump, npcs=npc_dump, throws=tdump,
        key_anims={k: anim_dump.get(k) for k in ["c0000:233000", "c0000:233001", "c0000:233300", "c0000:485100",
                                                 "c0000:233201", "c0000:233400", "c0000:233500", "c0000:233600",
                                                 "c0000:233800", "c0000:233801", "c0000:233810", "c0000:233900", "c2500:3000", "c2230:3000",
                                                 "c2230:3006", "c2232:3000", "c2232:3009"]},
        atk=atk_dump)
    if not args.no_clips:
        rep["clips"] = cook_clips(out_dir)
    rep["seconds"] = round(time.time() - t0, 1)
    (out_dir / "combat.json").write_text(json.dumps(rep, ensure_ascii=False, indent=1, default=str))
    print(f"combat.ashcb {len(body)} B, {rep['counts']}, unresolved behaviors {len(rep['unresolved_behaviors'])}, "
          f"{rep['seconds']} s")


CB_KIND_SIZE = 8 + 12 + 64
CB_ATK_SIZE = 16 + 8 + 8 + 4 + 16 + 16 + 8 + 2 + 6 + 4

# Knight clips for combat, cooked with tools/cook_knight.py's own functions (same skeleton and .ashanim format).
COMBAT_CLIPS = [("guard", "a00_0001", True), ("guard_hit", "a00_0100", False), ("guard_hit_l", "a00_0130", False),
                ("guard_break", "a00_0160", False),
                ("attack2", "a23_3001", False), ("attack3", "a23_3002", False), ("heavy", "a23_3300", False),
                ("heavy2", "a23_3301", False),
                ("parry", "a48_5100", False), ("riposte", "a23_3201", False), ("backstab", "a23_3400", False),
                ("hit_s", "a00_2000", False), ("hit_m", "a00_2004", False), ("hit_l", "a00_2010", False),
                ("knockdown", "a00_2014", False), ("parried", "a00_2100", False), ("death", "a00_6000", False),
                # 15.10 (combat polish). Longsword one-handed, AtkParam by the TAE windows (docs/combat.md):
                #   plunge start a23_3800 (falling hit 6-30, AtkParam 2300040, AttackType 62), the fall loop a23_3801
                #   (hit 0-39), the landing a23_3810 (hit 0-5, AtkParam 2300050, stamina 52);
                #   running R1 a23_3500 (2300520), running R2 / jump a23_3600 (2300030), rolling R1 a23_3900 (2300540)
                #   - roles by AtkParam_Pc row order and TAE root-motion events [версия]; repelled a00_2050 [версия]
                ("plunge", "a23_3800", False), ("plunge_fall", "a23_3801", True), ("plunge_land", "a23_3810", False),
                ("run_attack", "a23_3500", False), ("jump_attack", "a23_3600", False),
                ("roll_attack", "a23_3900", False), ("repelled", "a00_2050", False)]

# Extra hollow (c2500) clips for combat reactions, on the skeleton of tools/cook_enemy.py "hollow" (same bone order
# as hollow.ashc) -> hollow_combat.ashanim, a second AnimSet for that mesh. hollow.ashanim already has hit 2000,
# parried 2100, riposted 2510, backstabbed 2500, death 2200.
HOLLOW_CLIPS = [("hit_m", "a00_2004", False), ("hit_l", "a00_2010", False), ("knockdown", "a00_2014", False),
                ("repelled", "a00_2050", False)]


def cook_clips(out_dir: Path) -> dict:
    import cook_knight as ck
    sk = ck.Skeleton(ck.KEEP)
    hsk = ck.load_hkx_skeleton()
    clips, info = [], []
    for name, a, loop in COMBAT_CLIPS:
        try:
            c = ck.cook_clip(sk, hsk, a)
        except KeyError:
            info.append(dict(name=name, anim=a, error="not found"))
            print(f"  ! clip {a} not found", flush=True)
            continue
        qc = ck.quantize_clip(c)
        rec = ck.reconstruct(sk, qc, c["frames"])
        ref = np.einsum("ij,fkjl,lm->fkim", ck.S, c["arma"], ck.S)
        err = float(np.abs(rec[:, :, :3, 3] - ref[:, :, :3, 3]).max())
        clips.append((name, ck.anim_id(a), loop, c, qc))
        info.append(dict(name=name, anim=a, id=ck.anim_id(a), frames=c["frames"], src=c["src"],
                         joint_err_mm=round(err * 1000, 3), root_end=[round(float(x), 3) for x in c["root"][-1]]))
        print(f"  clip {name:12s} {c['src']:28s} frames={c['frames']:3d} err={err*1000:.2f} mm", flush=True)
    st = ck.write_ashanim(out_dir / "knight_combat.ashanim", len(sk.keep), clips)
    return dict(file=st, clips=info, hollow=cook_hollow_clips(out_dir))


def cook_hollow_clips(out_dir: Path) -> dict:
    import cook_enemy as ce
    cfg = ce.ENEMIES["hollow"]
    sk = ce.EnemySkeleton(cfg["chr"], cfg["keep"])
    sk.chr = cfg["chr"]
    b, hsk = ce.load_anibnd(cfg["chr"])
    clips, info = [], []
    for name, a, loop in HOLLOW_CLIPS:
        c = ce.cook_clip(sk, hsk, b, a)
        qc = ce.quantize_clip(c)
        rec = ce.reconstruct(sk, qc, c["frames"])
        ref = np.einsum("ij,fkjl,lm->fkim", ce.S, c["arma"], ce.S)
        err = float(np.abs(rec[:, :, :3, 3] - ref[:, :, :3, 3]).max())
        clips.append((name, ce.anim_id(a), loop, c, qc))
        info.append(dict(name=name, anim=a, id=ce.anim_id(a), frames=c["frames"], joint_err_mm=round(err * 1000, 3)))
        print(f"  hollow clip {name:10s} {a} frames={c['frames']:3d} err={err*1000:.2f} mm", flush=True)
    st = ce.write_ashanim(out_dir / "hollow_combat.ashanim", len(sk.keep), clips)
    return dict(file=st, clips=info)


if __name__ == "__main__":
    main()
