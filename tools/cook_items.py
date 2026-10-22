#!/usr/bin/env python3
"""Cook the item database of src/items (inventory, equipment, item use, loot) from the DSR copy.

    tools/.venv/bin/python tools/cook_items.py --out-dir ~/ash-assets/cooked/items [--png DIR]

Outputs (FromSoftware text and art -> outside git):
  items.ashit  tables for src/items/items.h (little-endian, layout = ItmHdr / ItmRec / ItmLot / ItmLotItem / ItmClass /
               ItmStart / ItmGraph; string pool UTF-8):
                 items    every item that a lot of m18_01 / m10_02, a class (CharaInitParam 3000..3009), a gift
                          (2400..2408) or the Asylum start can give, sorted by key = cat << 28 | id
                          (cat 0 weapon, 1 protector, 2 accessory, 4 goods - same key as src/event LotItem.item)
                 lots     ItemLotParam rows used by those maps: MSB treasures, EMEVD AwardItemLot (2003[36]) with
                          literal ids, Oscar's gift 1080, Asylum Demon drops 2660/2661, NpcParam.ItemLotID1 of MSB
                          characters (enemy drops, random by ChancePoints)
                 classes  10 classes + 9 gifts: attributes and the starting items with their equipment slot
                 graphs   CalcCorrectGraph rows the weapons scale with (attack rating at run time)
  items.ashui  item icons in the src/ui `.ashui` container (no font: 1 empty 8x4 atlas), image k = ItmRec.icon,
               cut from menu/menu_0|3.tpf Icon<X><Y> exactly as tools/cook_menu.py does (its item_icon is reused)
  items.json   counts, sizes, every item with its numbers, lots, warnings

Sources: GameParam EquipParamWeapon / Protector / Accessory / Goods, ItemLotParam, CharaInitParam, SpEffectParam
(what a goods item does when used, what a ring changes), CalcCorrectGraph, NpcParam; MSB m18_01 / m10_02; EMEVD of
both maps; FMG RUSSIAN item 10..13 (names), 24..27 (long captions, short 20..23 as fallback), menu 101 10010950
("Получено: ...") and 10010860 ("Использовано: ...").
"""
from __future__ import annotations

import argparse
import json
import logging
import os
import re
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
logging.disable(logging.WARNING)
import cook_menu as cm  # noqa: E402  (item_icon: the menu's icon cutter)
import cook_ui as cu    # noqa: E402

DSR = Path(os.environ.get("ASH_DSR", Path.home() / "ash-assets/dsr"))
MAPS = ("m18_01_00_00", "m10_02_00_00")
CLASS_ROWS = list(range(3000, 3010))
GIFT_ROWS = list(range(2400, 2409))
EXTRA_LOTS = {1080: "Oscar's gift (src/event cook_events.py)", 2660: "Asylum Demon drop", 2661: "Asylum Demon drop (special)"}
HILT = 212000             # «Рукоять прямого меча»: the Asylum start weapon [версия: DS1 по памяти, в PARAM этого нет]
ESTUS_GOODS = {200, 201}  # empty / full flask (+N rows are 202.. 215; only +0 is reachable in the slice)
EXTRA_ITEMS = [(4, 200), (4, 201), (0, HILT)]

CAT_BITS = {0: 0, 0x10000000: 1, 0x20000000: 2, 0x40000000: 4}
CAT_W, CAT_A, CAT_R, CAT_G = 0, 1, 2, 4

# src/items/items.h enums (keep in sync)
WK = dict(MELEE=0, SHIELD=1, BOW=2, XBOW=3, CATALYST=4, TALISMAN=5, FLAME=6, ARROW=7, BOLT=8)
GK = dict(CONSUMABLE=0, KEY=1, MATERIAL=2, ESTUS=3, OTHER=4)
F = dict(CONSUMED=1, QUICK=2, RIGHT=4, LEFT=8, TWOHAND=16, UNIQUE=32)
USE = dict(NONE=0, ESTUS=1, SOULS=2, HUMANITY=3, HEAL=4, THROW=5, SPEFFECT=6, KEY=7)
SLOT = dict(R1=0, R2=1, L1=2, L2=3, ARROW1=4, ARROW2=5, BOLT1=6, BOLT2=7, HEAD=8, BODY=9, ARMS=10, LEGS=11,
            RING1=12, RING2=13, Q1=14, Q2=15, Q3=16, Q4=17, Q5=18)
NONE8, NONE16 = 0xFF, 0xFFFF


def load_params():
    from soulstruct.darksouls1r.params.gameparambnd import GameParamBND
    g = GameParamBND.from_path(DSR / "param/GameParam/GameParam.parambnd.dcx")
    return {k.rsplit("\\", 1)[1]: v.rows for k, v in g.params.items()}


def weapon_kind(wid: int) -> int:
    """By EquipParamWeapon id range (DS1 numbering) [версия: по диапазонам id, поля «тип оружия» в PARAM DS1 нет]."""
    if 2000000 <= wid < 2100000:
        return WK["ARROW"]
    if 2100000 <= wid < 2200000:
        return WK["BOLT"]
    if 1400000 <= wid < 1600000:
        return WK["SHIELD"]
    if 1200000 <= wid < 1250000:
        return WK["BOW"]
    if 1250000 <= wid < 1300000:
        return WK["XBOW"]
    if 1300000 <= wid < 1330000:
        return WK["CATALYST"]
    if 1330000 <= wid < 1340000:
        return WK["FLAME"]
    if 1360000 <= wid < 1400000:
        return WK["TALISMAN"]
    return WK["MELEE"]


def i16(x) -> int:
    return max(-32768, min(32767, int(round(float(x)))))


class Cook:
    def __init__(self):
        self.P = load_params()
        self.fmg = cu.load_fmgs("RUSSIAN")
        self.warn: list[str] = []
        self.pool = bytearray(b"\0")
        self.pidx: dict[str, int] = {"": 0}
        self.icons: list = []
        self.icon_idx: dict[int, int] = {}

    # ---- text / icons
    def s(self, text: str | None) -> int:
        t = (text or "").replace("\r", "").strip()
        if t not in self.pidx:
            self.pidx[t] = len(self.pool)
            self.pool += t.encode("utf-8") + b"\0"
        return self.pidx[t]

    def item_fmg(self, f: int, i: int) -> str | None:
        return self.fmg.get(("item", f), {}).get(i)

    def icon(self, icon_id: int) -> int:
        if icon_id is None or icon_id < 0:
            return NONE16
        if icon_id not in self.icon_idx:
            im = cm.item_icon(icon_id)
            if im is None:
                self.warn.append(f"icon {icon_id} not found")
                self.icon_idx[icon_id] = NONE16
            else:
                self.icon_idx[icon_id] = len(self.icons)
                self.icons.append(cu.to_t8(im))
        return self.icon_idx[icon_id]

    # ---- lots
    def lot(self, lot_id: int):
        IL = self.P["ItemLotParam"]
        if lot_id not in IL:
            return None
        r = IL[lot_id]
        items = []
        for k in range(1, 9):
            iid = getattr(r, f"Item{k}ID")
            pts = getattr(r, f"Item{k}ChancePoints")
            if iid <= 0 and pts <= 0:
                continue
            cat = CAT_BITS.get(getattr(r, f"Item{k}Category"), 4)
            items.append((cat, max(0, iid), getattr(r, f"Item{k}Count") or 1, max(0, pts)))
        return dict(flag=r.ItemFlag, name=r.Name, items=items)

    def map_lots(self) -> dict[int, str]:
        from soulstruct.darksouls1r.events import EMEVD
        from soulstruct.darksouls1r.maps import MSB
        NP = self.P["NpcParam"]
        lots: dict[int, str] = {}
        for m in MAPS:
            msb = MSB.from_path(DSR / "map" / "MapStudio" / f"{m}.msb")
            for e in msb.get_events():
                if type(e).__name__ == "MSBTreasureEvent":
                    if "救済" in e.name:   # «спасательный» сундук для потерянных ключей (как docs/firelink.md)
                        continue
                    for x in (e.item_lot_1, e.item_lot_2):
                        if x and x > 0:
                            lots.setdefault(x, f"{m[:6]} treasure {e.name}")
            for c in msb.get_parts():
                if type(c).__name__ not in ("MSBCharacter",):
                    continue
                npc = getattr(c, "character_id", -1)
                if npc in NP:
                    for fld in (f"ItemLotID{k}" for k in range(1, 7)):
                        x = getattr(NP[npc], fld, -1)
                        if x and x > 0:
                            lots.setdefault(x, f"{m[:6]} drop {c.name} (NpcParam {npc})")
            try:
                ev = EMEVD.from_path(DSR / "event" / f"{m}.emevd.dcx")
                events = ev.events.values() if hasattr(ev.events, "values") else ev.events
                for e in events:
                    for ins in e.instructions:
                        if (ins.category, ins.index) == (2003, 36):
                            a = list(ins.args_list)
                            if a and isinstance(a[0], int) and a[0] > 0:
                                lots.setdefault(a[0], f"{m[:6]} EMEVD event {e.event_id} AwardItemLot")
            except Exception as ex:  # noqa: BLE001
                self.warn.append(f"EMEVD {m}: {ex}")
        for k, v in EXTRA_LOTS.items():
            lots.setdefault(k, v)
        return lots

    # ---- items
    def rec(self, cat: int, iid: int):
        """-> (key, packed 64 B record, json dict) or None."""
        P = self.P
        key = (cat << 28) | iid
        v = [0] * 18
        flags = use = use_val = kind = 0
        max_hold = 1
        if cat == CAT_W:
            W = P["EquipParamWeapon"]
            if iid not in W:
                return None
            r = W[iid]
            name, desc = self.item_fmg(11, iid), self.item_fmg(25, iid) or self.item_fmg(21, iid)
            icon, weight = r.WeaponIcon, r.Weight
            kind = weapon_kind(iid)
            v[0:4] = [r.BasePhysicalDamage, r.BaseMagicDamage, r.BaseFireDamage, r.BaseLightningDamage]
            v[4:8] = [r.StrengthScaling, r.DexterityScaling, r.IntelligenceScaling, r.FaithScaling]
            v[8:12] = [r.RequiredStrength, r.RequiredDexterity, r.RequiredIntelligence, r.RequiredFaith]
            v[12:16] = [r.PhysicalGuardPercentage, r.MagicGuardPercentage, r.FireGuardPercentage,
                        r.LightningGuardPercentage]
            v[16] = r.GuardStaminaDefense
            v[17] = getattr(r, "ScalingFormulaType", 0)
            flags |= F["RIGHT"] if r.RightHandAllowed else 0
            flags |= F["LEFT"] if r.LeftHandAllowed else 0
            flags |= F["TWOHAND"] if r.BothHandsAllowed else 0
            if kind in (WK["ARROW"], WK["BOLT"]):
                max_hold = 99
                flags &= ~(F["RIGHT"] | F["LEFT"] | F["TWOHAND"])
            self.graphs.add(v[17])
        elif cat == CAT_A:
            A = P["EquipParamProtector"]
            if iid not in A:
                return None
            r = A[iid]
            name, desc = self.item_fmg(12, iid), self.item_fmg(26, iid) or self.item_fmg(22, iid)
            icon, weight = r.MaleIcon, r.Weight
            kind = 0 if r.EquippedToHead else 1 if r.EquippedToBody else 2 if r.EquippedToHands else 3
            v[0:7] = [r.PhysicalDefense, r.MagicDefense, r.FireDefense, r.LightningDefense, r.StrikeDefense,
                      r.SlashDefense, r.ThrustDefense]
            v[7] = r.Poise
            v[8:11] = [r.BleedResistance, r.PoisonResistance, r.CurseResistance]
        elif cat == CAT_R:
            A = P["EquipParamAccessory"]
            if iid not in A:
                return None
            r = A[iid]
            name, desc = self.item_fmg(13, iid), self.item_fmg(27, iid) or self.item_fmg(23, iid)
            icon, weight = r.MenuIcon, r.Weight
            sp = P["SpEffectParam"].get(r.SpecialEffect) if hasattr(P["SpEffectParam"], "get") else None
            g = (lambda f, d: getattr(sp, f, d)) if sp is not None else (lambda f, d: d)
            v[0] = g("MaxEquipLoadMultiplier", 1.0) * 100
            v[1] = g("MaxHPMultiplier", 1.0) * 100
            v[2] = g("MaxStaminaMultiplier", 1.0) * 100
            v[3] = g("PoiseAddition", 0)
            v[4:8] = [g("PhysicalDefenseAddition", 0), g("MagicDefenseAddition", 0), g("FireDefenseAddition", 0),
                      g("LightningDefenseAddition", 0)]
            v[8] = g("StaminaRecoverySpeedChange", 0)
            use_val = r.SpecialEffect
        elif cat == CAT_G:
            G = P["EquipParamGoods"]
            if iid not in G:
                return None
            r = G[iid]
            name, desc = self.item_fmg(10, iid), self.item_fmg(24, iid) or self.item_fmg(20, iid)
            icon, weight = r.GoodIcon, r.Weight
            max_hold = max(1, r.MaxHoldQuantity)
            flags |= F["CONSUMED"] if r.ConsumedOnUse else 0
            flags |= F["QUICK"] if r.CanBeEquipped else 0
            flags |= F["UNIQUE"] if r.IsUnique else 0
            gt = r.GoodType
            kind = GK["KEY"] if gt == 1 else GK["MATERIAL"] if gt == 2 else GK["CONSUMABLE"] if gt == 0 else GK["OTHER"]
            v[0] = r.UseAnimation
            v[1] = r.GoodToReplace if r.GoodToReplace > 0 else -1
            if r.IsEmptyEstusFlask or iid in ESTUS_GOODS:
                kind, use = GK["ESTUS"], USE["ESTUS"]
                flags |= F["QUICK"]
                flags &= ~F["CONSUMED"]       # charges live in src/progress (Progress.estus)
                max_hold = 1
                use_val = r.ReferenceID if r.ReferenceType == 2 else 3020
            elif gt == 1:
                use = USE["KEY"]
            elif r.ReferenceType == 1 and r.ReferenceID > 0:          # Bullet: thrown (fire bomb, dung pie ...)
                use, use_val = USE["THROW"], r.ReferenceID
            elif r.ReferenceType == 2 and r.ReferenceID > 0:          # SpEffect on the user
                sp = P["SpEffectParam"][r.ReferenceID] if r.ReferenceID in P["SpEffectParam"] else None
                use, use_val = USE["SPEFFECT"], r.ReferenceID
                if sp is not None:
                    if getattr(sp, "SoulAmountChange", 0) > 0:
                        use, use_val = USE["SOULS"], sp.SoulAmountChange
                    elif getattr(sp, "SpecialState", 0) == 119 or getattr(sp, "HumanityDamage", 0) < 0:
                        # humanity: HumanityDamage -2 = twin humanity; 0 with SpecialState 119 = +1 [версия]
                        use, use_val = USE["HUMANITY"], max(1, -getattr(sp, "HumanityDamage", 0))
                    elif getattr(sp, "HPPointsLost", 0) < 0:
                        use, use_val = USE["HEAL"], -sp.HPPointsLost
        else:
            return None
        if not name:
            self.warn.append(f"no FMG name for cat {cat} id {iid}")
        icon_i = self.icon(icon)
        rec = struct.pack("<3I2H4Bfi18h", key, self.s(name), self.s(desc), icon_i, max_hold, cat, kind, flags, use,
                          float(weight), int(use_val), *[i16(x) for x in v])
        assert len(rec) == 64
        return key, rec, dict(key=f"{cat}:{iid}", name=name, cat=cat, kind=kind, flags=flags, use=use,
                              use_val=int(use_val), weight=round(float(weight), 2), max_hold=max_hold,
                              icon=icon, v=[i16(x) for x in v])

    def run(self, out: Path, png: Path | None):
        P = self.P
        self.graphs: set[int] = set()
        CI = P["CharaInitParam"]
        want: dict[tuple[int, int], str] = {}

        # lots
        lot_src = self.map_lots()
        lots, lotitems = [], []
        lot_dump = []
        for lid in sorted(lot_src):
            L = self.lot(lid)
            if L is None:
                self.warn.append(f"ItemLot {lid} missing ({lot_src[lid]})")
                continue
            first = len(lotitems)
            for cat, iid, cnt, pts in L["items"]:
                if iid > 0:
                    want.setdefault((cat, iid), f"lot {lid}")
                lotitems.append((cat, iid, cnt, pts))
            lots.append((lid, L["flag"], first, len(L["items"])))
            lot_dump.append(dict(lot=lid, flag=L["flag"], src=lot_src[lid], items=[
                dict(item=f"{c}:{i}", count=n, chance=p, name=None) for c, i, n, p in L["items"]]))

        # classes and gifts
        starts, classes, cls_dump = [], [], []
        for row in CLASS_ROWS + GIFT_ROWS:
            r = CI[row]
            first = len(starts)
            gift = row in GIFT_ROWS

            def add(cat, iid, cnt, slot, asylum):
                if iid is None or iid <= 0:
                    return
                want.setdefault((cat, iid), f"CharaInitParam {row}")
                starts.append((cat, iid, cnt, slot, asylum))
            if not gift:
                # Asylum start [версия]: class armour + goods, the Straight Sword Hilt in R1; the class weapon and shield
                # are picked up near bonfire 2 (src/event lots 1810100..1810330), Estus comes from Oscar (lot 1080).
                add(CAT_W, getattr(r, "RightHandWeapon1"), 1, SLOT["R1"], 0)
                add(CAT_W, getattr(r, "RightHandWeapon2"), 1, SLOT["R2"], 0)
                add(CAT_W, getattr(r, "LeftHandWeapon1"), 1, SLOT["L1"], 0)
                add(CAT_W, getattr(r, "LeftHandWeapon2"), 1, SLOT["L2"], 0)
                add(CAT_W, HILT, 1, SLOT["R1"], 2)          # asylum = 2: only in the Asylum start
                for fld, sl in (("HeadArmor", "HEAD"), ("BodyArmor", "BODY"), ("HandsArmor", "ARMS"),
                                ("LegsArmor", "LEGS")):
                    pid = getattr(r, fld)
                    pr = P["EquipParamProtector"].get(pid) if hasattr(P["EquipParamProtector"], "get") else None
                    if pid > 0 and pr is not None and pr.EquipmentModel != 0:      # 0 = bare (Deprived's "Голова")
                        add(CAT_A, pid, 1, SLOT[sl], 1)
                for k, fld in enumerate(("ArrowSlot1", "ArrowSlot2")):
                    add(CAT_W, getattr(r, fld), getattr(r, fld + "Count"), SLOT["ARROW1"] + k, 0)
                for k, fld in enumerate(("BoltSlot1", "BoltSlot2")):
                    add(CAT_W, getattr(r, fld), getattr(r, fld + "Count"), SLOT["BOLT1"] + k, 0)
                add(CAT_G, 201, 5, SLOT["Q1"], 0)            # Estus x5 in the full start [версия: DS1 стартовые 5]
            q = 1
            for k in range(1, 11):
                gid = getattr(r, f"GoodSlot{k}")
                if gid > 0:
                    gq = P["EquipParamGoods"][gid].CanBeEquipped if gid in P["EquipParamGoods"] else 0
                    slot = NONE8
                    if gq and not gift and q < 5:
                        slot = SLOT["Q1"] + q
                        q += 1
                    add(CAT_G, gid, getattr(r, f"GoodSlot{k}Count") or 1, slot, 1)
            for k in range(1, 6):
                add(CAT_R, getattr(r, f"RingSlot{k}"), 1, (SLOT["RING1"] + k - 1) if k <= 2 and not gift else NONE8, 1)
            attrs = [r.Vitality, r.Attunement, r.Endurance, r.Strength, r.Dexterity, r.Resistance, r.Intelligence,
                     r.Faith] if not gift else [0] * 8
            classes.append(struct.pack("<H2B8B2H", row, r.Level & 0xFF if not gift else 0,
                                       r.Humanity & 0xFF if not gift else 0, *attrs, first, len(starts) - first))
            cls_dump.append(dict(row=row, level=r.Level, attrs=attrs, start=[
                dict(item=f"{c}:{i}", count=n, slot=s, asylum=a) for c, i, n, s, a in starts[first:]]))
        for cat, iid in EXTRA_ITEMS:
            want.setdefault((cat, iid), "Ash")

        # item records
        recs = {}
        dump = []
        for (cat, iid), src in sorted(want.items(), key=lambda kv: (kv[0][0] << 28) | kv[0][1]):
            r = self.rec(cat, iid)
            if r is None:
                self.warn.append(f"item {cat}:{iid} not in PARAM ({src})")
                continue
            recs[r[0]] = r[1]
            r[2]["src"] = src
            dump.append(r[2])
        names = {d["key"]: d["name"] for d in dump}
        for L in lot_dump:
            for it in L["items"]:
                it["name"] = names.get(it["item"])

        # graphs
        CG = P["CalcCorrectGraph"]
        graphs = []
        gids = sorted(g for g in self.graphs if g in CG) or [0]
        for gid in gids:
            g = CG[gid]
            xs = [getattr(g, f"StageMaxIntercept{i}") for i in range(5)]
            ys = [getattr(g, f"StageMaxSlope{i}", 0.0) for i in range(5)]
            es = [getattr(g, f"AdjustmentMaxSlope{i}", 1.0) for i in range(5)]
            graphs.append(struct.pack("<2H15f", gid, 0, *xs, *ys, *es))

        # texts
        got = (self.fmg.get(("menu", 101), {}).get(10010950) or "Получено: ")
        used = (self.fmg.get(("menu", 101), {}).get(10010860) or "Использовано: ")
        got_pre = re.sub(r"<\?gdsparam@\d+\?>.*$", "", got, flags=re.S)
        used_pre = re.sub(r"<\?gdsparam@\d+\?>.*$", "", used, flags=re.S)
        s_got, s_used = self.s(got_pre + "\x01"), self.s(used_pre + "\x01")   # \x01 = where the name goes
        s_full = self.s("Нельзя унести больше")                                 # [придумал]

        # pack
        keys = sorted(recs)
        body_items = b"".join(recs[k] for k in keys)
        body_lots = b"".join(struct.pack("<2I2H", lid & 0xFFFFFFFF, flag & 0xFFFFFFFF, first, n)
                             for lid, flag, first, n in lots)
        body_li = b"".join(struct.pack("<I2H", (c << 28) | i, min(cnt, 0xFFFF), min(p, 0xFFFF))
                           for c, i, cnt, p in lotitems)
        body_cls = b"".join(classes)
        body_st = b"".join(struct.pack("<IH2B", (c << 28) | i, min(n, 0xFFFF), s, a) for c, i, n, s, a in starts)
        body_g = b"".join(graphs)
        sections = [body_items, body_lots, body_li, body_cls, body_st, body_g, bytes(self.pool)]
        offs, cur = [], 80
        for s in sections:
            offs.append(cur)
            cur += len(s)
            cur += (-cur) % 4
        hdr = struct.pack("<4sI18I", b"ASIT", 1,
                          len(keys), offs[0], len(lots), offs[1], len(lotitems), offs[2], len(classes), offs[3],
                          len(starts), offs[4], len(graphs), offs[5], offs[6], len(self.pool), s_got, s_used, s_full, 0)
        assert len(hdr) == 80, len(hdr)
        blob = bytearray(hdr)
        for o, s in zip(offs, sections):
            blob += b"\0" * (o - len(blob))
            blob += s
        blob += b"\0" * ((-len(blob)) % 4)
        out.mkdir(parents=True, exist_ok=True)
        (out / "items.ashit").write_bytes(bytes(blob))
        font = dict(W=8, H=4, pix=b"\0" * 16, fonts=[], glyphs=[])
        st = cu.write(out / "items.ashui", font, [], self.icons)
        info = dict(ashit_bytes=len(blob), ashui_bytes=st["bytes"], items=len(keys), lots=len(lots),
                    lot_items=len(lotitems), classes=len(classes), starts=len(starts), graphs=gids,
                    icons=len(self.icons), string_pool=len(self.pool), got_prefix=got_pre, used_prefix=used_pre,
                    warnings=self.warn, item_list=dump, lot_list=lot_dump, class_list=cls_dump)
        (out / "items.json").write_text(json.dumps(info, ensure_ascii=False, indent=1))
        if png:
            from PIL import Image
            png.mkdir(parents=True, exist_ok=True)
            tiles = [i["preview"] for i in self.icons]
            cols = 16
            sheet = Image.new("RGBA", (cols * 34, ((len(tiles) + cols - 1) // cols) * 34), (40, 30, 40, 255))
            for k, t in enumerate(tiles):
                sheet.alpha_composite(t.convert("RGBA").resize((32, 32)), ((k % cols) * 34 + 1, (k // cols) * 34 + 1))
            sheet.save(png / "items_icons.png")
        return info


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out-dir", required=True)
    ap.add_argument("--png", help="icon sheet preview -> this dir (outside git)")
    a = ap.parse_args()
    cm.LANG = "RUSSIAN"
    info = Cook().run(Path(a.out_dir).expanduser(), Path(a.png).expanduser() if a.png else None)
    print(f"items.ashit {info['ashit_bytes']} B, items.ashui {info['ashui_bytes']} B: {info['items']} items, "
          f"{info['lots']} lots ({info['lot_items']} entries), {info['classes']} classes+gifts, {info['icons']} icons, "
          f"graphs {info['graphs']}")
    for w in info["warnings"]:
        print("warn:", w)


if __name__ == "__main__":
    main()
