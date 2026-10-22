#!/usr/bin/env python3
"""Cook the event script of one DSR map (EMEVD, via Soulstruct) into `.ashevt` for src/event/ (docs/events.md).

    tools/.venv/bin/python tools/cook_events.py --map m18_01_00_00 --out ~/ash-assets/cooked/m18_01.ashevt
    tools/.venv/bin/python tools/cook_events.py --gen-header src/event/evops.h     # opcode enum for the C side
    tools/.venv/bin/python tools/cook_events.py --list                             # instances + decisions

What it does
  1. Reads `event/<map>.emevd.dcx` and *flattens* it: every RunEvent(slot, event, args) of the constructor (0) and
     preconstructor (50) becomes one instance with the event-argument replacements already applied (byte-exact, the
     same packing the game uses: natural alignment, `struct_args_fmt`). The runtime therefore needs no arg patching.
  2. Converts each EMEVD instruction 1:1 into an Ash instruction (opcode + 32-bit words). 1:1 matters: skip counts
     are in lines. Things that are constant for us (single player, host, NG, always in m18_01, class) are folded
     at cook time; unsupported ones become NOP. Nothing is reordered.
  3. Applies the cut-down ("урезание") overrides below (cutscenes -> fade / end of demo, Snuggly/Oscar/NG+ dropped)
     and appends Ash-authored events (ids 9181xxxx: Estus at Oscar's spot, iron-ball roll, firebomb trap).
  4. Writes the tables the interpreter needs: entities (regions, objects, characters, collisions, spawn points) in
     PSP space (x mirrored, yaw negated, like tools/cook_scene.py), treasures, item lots, object activations
     (doors + keys), boss drops and the Russian strings (prompts, item / boss names).

`.ashevt` v1, little-endian, every table 4-aligned, offsets from file start (C side: src/event/evfile.h):
  Header 96 B : "AEVT", u32 version=1, then 11 (count, offset) pairs: inst, code(words), ent, treas, lot, lotitem,
                str, pool(bytes), root, objact, drop
  Inst  16 B  : u32 event_id, u16 slot, u8 rest (0 ContinueOnRest, 1 RestartOnRest), u8 flags, u32 code_off, u32 n
  Code        : per instruction u32 (op | nargs << 16), then nargs u32 words (int or float bits; types per opcode)
  Ent   48 B  : u32 id, u8 kind, u8 shape, u16 str, f32 pos[3], f32 yaw (rad, PSP space), f32 dim[3], u32 model,
                u32 extra, u32 extra2
  Treas 32 B  : u32 obj, u32 lot[2], f32 pos[3], u8 hidden, u8 pad[3], u32 extra
  Lot   12 B  : u32 lot_id, u32 flag, u16 first, u16 n
  LotItem 8 B : u32 item (cat << 28 | id; cat 0 weapon 1 armor 2 ring 4 goods), u16 count, u16 str (index)
  Str    8 B  : u32 key, u32 off (into pool, UTF-8, NUL-terminated); key = kind << 28 | id (0 text, 1 npc name,
                2 item name: 2 << 28 | cat << 24 | id, 3 part name of an entity: 3 << 28 | entity id), sorted by key
  Root   4 B  : u32 instance index, run in order at (re)load
  ObjAct 32 B : u32 objact_id, u32 obj, u32 prompt, u32 fail, u32 need (item key or speffect), u8 need_type,
                u8 pad[3], u32 auto_flag, u32 done_flag
  Drop   8 B  : u32 chr, u32 lot
Never committed (contains FromSoftware data: ids, positions, strings).
"""
from __future__ import annotations

import argparse
import json
import logging
import math
import re
import struct
import sys
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
logging.disable(logging.WARNING)
from dsrlib import DSR, Binder  # noqa: E402

PLAYER = 10000

# --------------------------------------------------------------------------------------------- opcodes
# Single source of truth for src/event/evops.h (--gen-header). Order = numeric value. Append only.
# Arg words after the name: i = int, f = float. Condition ops take the condition group first.
OPS = [
    ("NOP", ""),
    # conditions (first word = group: 0 MAIN, 1..8 AND, -1..-7 OR)
    ("C_CONST", "ii"),            # group, value
    ("C_COND", "iii"),            # group, state, input group
    ("C_TIME", "if"),             # group, seconds since instance start
    ("C_FLAG", "iiii"),           # group, state, flag_type, flag
    ("C_REGION", "iiii"),         # group, state(1 inside), chr, region
    ("C_ACTION", "iiiffiii"),     # group, anchor_type(0 obj 1 region 2 chr), anchor, angle, dist, prompt, line_obj, boss
    ("C_DEAD", "iii"),            # group, chr, is_dead
    ("C_ATTACKED", "iii"),        # group, target, attacker
    ("C_HP", "iiif"),             # group, chr, cmp, ratio
    ("C_OBJ_DESTROYED", "iii"),   # group, state, obj
    ("C_OBJ_ACTIVATED", "ii"),    # group, objact_id
    ("C_ON_COLLISION", "ii"),     # group, collision entity
    # control
    ("SKIP", "i"),
    ("SKIP_IF_COND", "iii"),      # lines, state, group
    ("RET_IF_COND", "iii"),       # type(0 end 1 restart), state, group
    ("RET", "i"),
    ("SKIP_IF_CMP", "iiii"),      # lines, cmp, left, right
    ("RET_IF_CMP", "iiii"),
    ("SKIP_IF_LAST", "iii"),      # lines, state, group
    ("RET_IF_LAST", "iii"),
    ("SKIP_IF_FLAG", "iiii"),     # lines, state, flag_type, flag
    ("RET_IF_FLAG", "iiii"),      # type, state, flag_type, flag
    ("WAIT", "f"),
    ("WAIT_FRAMES", "i"),
    ("RUN", "i"),                 # instance index
    # flags / progress
    ("FLAG", "ii"),               # flag, state (0 off, 1 on, 2 toggle)
    ("FLAG_RANGE", "iii"),
    ("RESPAWN", "i"),             # spawn point entity
    ("SAVE", ""),
    ("AWARD_LOT", "i"),
    # characters
    ("CHR_SET", "iii"),           # chr, what (EV_CHR_*), value
    ("KILL", "ii"),               # chr, award souls
    ("ANIM", "iiii"),             # entity (chr or obj), anim, loop, wait
    ("FACE", "ii"),               # chr, target entity
    ("MOVE", "iii"),              # chr, dest_type, dest
    ("STANDBY", "ii"),            # chr, anim (-1 reset)
    ("SPEFFECT", "iii"),          # chr, effect, add
    ("NEST", "ii"),
    ("AI_CMD", "iii"),
    ("BOSS_BAR", "iii"),          # state, chr, name id
    ("KILL_BOSS", "i"),
    ("BOSS_START", ""),
    # objects
    ("OBJ_STATE", "ii"),          # obj, enabled
    ("OBJ_DESTROY", "i"),
    ("OBJ_RESTORE", "i"),
    ("OBJ_END_ANIM", "ii"),       # obj, anim (jump to its end state)
    ("OBJ_ACT", "ii"),            # obj, enabled
    ("TREASURE", "ii"),           # obj, enabled
    ("VFX", "ii"),                # vfx entity, on
    ("VFX_TEMP", "iii"),          # anchor_type, anchor, vfx id
    ("MAPCOL", "ii"),             # collision entity, enabled
    ("HAZARD", "iiiff"),          # obj_flag, obj, behavior, radius, life
    # presentation
    ("DIALOG", "ii"),             # text, anchor
    ("CUTSCENE", "iii"),          # id, move_to region (0 none), wait frames hint
    ("SOUND", "ii"),              # sound event, on
    ("SFX", "iii"),               # anchor, type, id
    ("LADDER", "iii"),            # start flag, stop flag, obj
    ("BONFIRE", "iiff"),          # flag, obj, distance, angle
    # Ash additions (no EMEVD counterpart)
    ("END_DEMO", "i"),            # text id (0 = default "Конец демо")
    ("FADE", "ff"),               # out seconds, in seconds
    ("BALL", "iiff"),             # obj, wall obj to break, speed, radius
    ("BOMB", "iiif"),             # thrower chr, target chr, count, interval
    ("BANNER", "i"),              # text key shown centred (host)
    ("TREASURE_ADD", "i"),        # treasure index (Ash-authored loot) enabled
]
OP = {name: i for i, (name, _) in enumerate(OPS)}
CHR_WHAT = ["ENABLE", "AI", "INVINCIBLE", "HPBAR", "TEAM", "BACKREAD"]
CHR = {n: i for i, n in enumerate(CHR_WHAT)}

EK = dict(REGION=1, OBJECT=2, CHR=3, COLL=4, SPAWN=5, VFX=6, POINT=7)
SHAPE = dict(POINT=0, SPHERE=1, CYLINDER=2, BOX=3)

# Constants of our single-player slice (folded at cook time).
CLASS_DEFAULT = 0          # Warrior: longsword + heater shield = the weapon tools/cook_knight.py animates [придумал]
CHR_TYPE_PLAYER = 8        # Hollow (CharacterType) — the player wakes up hollow
WORLD_TENDENCY = 0

# Strings that are not in the game's FMG for these actions (our own text) [придумал].
ASH_TEXT = {
    900001: "Поднять",
    900002: "Конец демо",
    900003: "Северное прибежище нежити",
    900004: "ВЫ ПОГИБЛИ",
    900005: "ВРАГ ПОВЕРЖЕН",
}


# --------------------------------------------------------------------------------------------- helpers

def mirror_pos(p):
    return [-float(p[0]), float(p[1]), float(p[2])]


def mirror_yaw_deg(y):
    """FromSoft yaw (deg, about +Y) -> PSP space radians (x mirrored -> rotation sign flips)."""
    return -math.radians(float(y))


def model_num(name: str | None) -> int:
    if not name:
        return 0
    try:
        return int(name[1:5])
    except ValueError:
        return 0


def rot_y(v, deg):
    """Rotate a local FromSoft vector by MSB yaw (FromSoft Ry, same as tools/cook_collision.euler_matrix)."""
    a = math.radians(deg)
    c, s = math.cos(a), math.sin(a)
    x, y, z = v
    return [c * x + s * z, y, -s * x + c * z]


class Ctx:
    def __init__(self, map_id: str, class_type: int):
        from soulstruct.darksouls1r.events import EMEVD
        from soulstruct.darksouls1r.maps import MSB
        from soulstruct.darksouls1r.params import GameParamBND
        from soulstruct.darksouls1r.text import MSGDirectory
        self.map_id = map_id
        self.class_type = class_type
        self.emevd = EMEVD.from_path(DSR / "event" / f"{map_id}.emevd.dcx")
        self.msb = MSB.from_path(DSR / "map" / "MapStudio" / f"{map_id}.msb")
        gp = GameParamBND.from_path(DSR / "param/GameParam/GameParam.parambnd.dcx")
        self.P = lambda n: next(v for k, v in gp.params.items() if k.endswith("\\" + n))
        self.msg = MSGDirectory.from_path(DSR / "msg/RUSSIAN")
        self.area = int(map_id[1:3])
        self.block = int(map_id[4:6])
        self.strings: dict[int, str] = {}
        self.lots_used: set[int] = set()
        self.warn: list[str] = []
        self.unsupported: dict[str, int] = {}

    # ---- text
    def text(self, tid: int) -> int:
        if tid <= 0:
            return 0
        if tid in ASH_TEXT:
            self.strings[tid] = ASH_TEXT[tid]
            return tid
        s = self.msg.EventText.get(tid) or (self.msg.EventTextPatch.get(tid) if self.msg.EventTextPatch else None)
        if s:
            # "<?gdsparam@2010?>" -> goods name (the game fills these placeholders at runtime)
            s = re.sub(r"<\?gdsparam@(\d+)\?>", lambda m: self.msg.GoodNames.get(int(m.group(1))) or "?", s)
            self.strings[tid] = s
        return tid

    def npc_name(self, nid: int) -> int:
        s = self.msg.NPCNames.get(nid)
        if s:
            self.strings[(1 << 28) | nid] = s
        return nid

    def item_name(self, cat: int, iid: int) -> int:
        tab = {0: "WeaponNames", 1: "ArmorNames", 2: "RingNames", 4: "GoodNames"}.get(cat)
        s = getattr(self.msg, tab).get(iid) if tab else None
        key = (2 << 28) | ((cat & 7) << 24) | (iid & 0xFFFFFF)
        if s:
            self.strings[key] = s
        return key


# --------------------------------------------------------------------------------------------- flatten

def apply_replacements(ins, blob: bytes) -> list:
    args = list(ins.args_list)
    if not ins.event_arg_replacements:
        return args
    fmt = "@" + ins.struct_args_fmt
    raw = bytearray(struct.pack(fmt, *args))
    for r in ins.event_arg_replacements:
        raw[r.write_offset:r.write_offset + r.size] = blob[r.read_offset:r.read_offset + r.size]
    return list(struct.unpack(fmt, bytes(raw)))


class Instance:
    def __init__(self, eid, slot, rest, blob, args_repr):
        self.eid, self.slot, self.rest, self.blob, self.args_repr = eid, slot, rest, blob, args_repr
        self.code: list[tuple[int, list]] = []   # (op, [(type, value)])
        self.note = ""
        self.decision = "делаем"


def flatten(ctx: Ctx):
    """Instances reachable from events 50 and 0 with their RunEvent args."""
    ev = ctx.emevd.events
    insts: list[Instance] = []
    index: dict[tuple, int] = {}

    def add(eid, slot, blob, args_repr):
        key = (eid, slot, blob)
        if key in index:
            return index[key]
        e = ev[eid]
        rest = 0 if e.on_rest_behavior.value == 0 else 1
        inst = Instance(eid, slot, rest, blob, args_repr)
        index[key] = len(insts)
        insts.append(inst)
        inst.raw = [(ins, apply_replacements(ins, blob)) for ins in e.instructions]
        for ins, args in inst.raw:
            if (ins.category, ins.index) == (2000, 0):
                fmt = "@" + ins.struct_args_fmt
                sub_blob = struct.pack(fmt, *args)[8:]
                add(args[1], args[0], sub_blob, args[2:])
        return index[key]

    roots = [add(50, 0, b"", []), add(0, 0, b"", [])]
    return insts, index, roots


# --------------------------------------------------------------------------------------------- convert

def W(t, v):
    return (t, v)


def conv(ctx: Ctx, key, a, inst_index) -> tuple[str, list]:
    """One EMEVD instruction -> (op name, [(type, value)]). Must return exactly one instruction."""
    I = lambda *vs: [W("i", int(v)) for v in vs]  # noqa: E731
    c = key
    # ---------------- conditions
    if c == (0, 0):
        return "C_COND", I(a[0], a[1], a[2])
    if c == (1, 0):
        return "C_TIME", I(a[0]) + [W("f", a[1])]
    if c == (3, 0):
        return "C_FLAG", I(a[0], a[1], a[2], a[3])
    if c == (3, 2):
        return "C_REGION", I(a[0], a[1], a[2], a[3])
    if c in ((3, 5), (3, 18), (3, 19)):
        # cond, anchor_type, anchor, facing_angle, dummy, max_distance, prompt, trigger_attr, button[, line]
        line = a[9] if len(a) > 9 else 0
        ctx.text(a[6])
        return "C_ACTION", I(a[0], a[1], a[2]) + [W("f", a[3]), W("f", a[5])] + I(a[6], line, c == (3, 19))
    if c == (3, 6):   # multiplayer state: Host 0 / Singleplayer 3 true
        return "C_CONST", I(a[0], a[1] in (0, 3))
    if c == (3, 8):   # map presence
        return "C_CONST", I(a[0], (a[2] == ctx.area and a[3] == ctx.block) == bool(a[1]))
    if c == (3, 11):  # world tendency comparison (fixed 0)
        return "C_CONST", I(a[0], cmp_const(a[2], WORLD_TENDENCY, a[3]))
    if c == (3, 14):  # any item dropped in region (Snuggly trading) — never
        return "C_CONST", I(a[0], 0)
    if c == (3, 15):
        return "C_CONST", I(a[0], 0)
    if c == (3, 17):  # NG+ cycle comparison (always NG)
        return "C_CONST", I(a[0], cmp_const(a[1], 0, a[2]))
    if c == (4, 0):
        return "C_DEAD", I(a[0], a[1], a[2])
    if c == (4, 1):
        return "C_ATTACKED", I(a[0], a[1], a[2])
    if c == (4, 2):
        return "C_HP", I(a[0], a[1], a[2]) + [W("f", a[3])]
    if c == (4, 3):   # character is type (player only here)
        return "C_CONST", I(a[0], a[1] == PLAYER and a[2] == CHR_TYPE_PLAYER)
    if c == (4, 7):   # backread state — every chr we keep is "loaded"
        return "C_CONST", I(a[0], bool(a[2]))
    if c == (4, 11):
        return "C_CONST", I(a[0], a[1] == ctx.class_type)
    if c == (4, 12):  # covenant — none
        return "C_CONST", I(a[0], 0)
    if c == (5, 0):
        return "C_OBJ_DESTROYED", I(a[0], a[1], a[2])
    if c == (5, 2):
        return "C_OBJ_ACTIVATED", I(a[0], a[1])
    if c == (11, 2):
        return "C_ON_COLLISION", I(a[0], a[1])
    # ---------------- control
    if c == (1000, 1):
        return "SKIP_IF_COND", I(a[0], a[1], a[2])
    if c == (1000, 2):
        return "RET_IF_COND", I(a[0], a[1], a[2])
    if c == (1000, 3):
        return "SKIP", I(a[0])
    if c == (1000, 4):
        return "RET", I(a[0])
    if c == (1000, 5):
        return "SKIP_IF_CMP", I(a[0], a[1], a[2], a[3])
    if c == (1000, 6):
        return "RET_IF_CMP", I(a[0], a[1], a[2], a[3])
    if c == (1000, 7):
        return "SKIP_IF_LAST", I(a[0], a[1], a[2])
    if c == (1000, 8):
        return "RET_IF_LAST", I(a[0], a[1], a[2])
    if c == (1001, 0):
        return "WAIT", [W("f", a[0])]
    if c == (1001, 1):
        return "WAIT_FRAMES", I(a[0])
    if c == (1003, 1):
        return "SKIP_IF_FLAG", I(a[0], a[1], a[2], a[3])
    if c == (1003, 2):
        return "RET_IF_FLAG", I(a[0], a[1], a[2], a[3])
    if c == (1003, 5):   # skip if multiplayer state
        return ("SKIP", I(a[0])) if a[1] in (0, 3) else ("NOP", [])
    if c == (1003, 6):
        return ("RET", I(a[0])) if a[1] in (0, 3) else ("NOP", [])
    if c == (1003, 7):   # skip if map presence state
        here = a[2] == ctx.area and a[3] == ctx.block
        return ("SKIP", I(a[0])) if here == bool(a[1]) else ("NOP", [])
    if c == (2000, 0):
        return "RUN", I(inst_index)  # patched by caller
    # ---------------- actions
    if c == (2003, 2):
        return "FLAG", I(a[0], a[1])
    if c == (2003, 22):
        return "FLAG_RANGE", I(a[0], a[1], a[2])
    if c == (2003, 23):
        return "RESPAWN", I(a[0])
    if c == (2000, 5):
        return "SAVE", []
    if c in ((2003, 36),):
        ctx.lots_used.add(a[0])
        return "AWARD_LOT", I(a[0])
    if c == (2004, 5):
        return "CHR_SET", I(a[0], CHR["ENABLE"], a[1])
    if c == (2004, 1):
        return "CHR_SET", I(a[0], CHR["AI"], a[1])
    if c == (2004, 15):
        return "CHR_SET", I(a[0], CHR["INVINCIBLE"], a[1])
    if c == (2004, 30):
        return "CHR_SET", I(a[0], CHR["HPBAR"], a[1])
    if c == (2004, 2):
        return "CHR_SET", I(a[0], CHR["TEAM"], a[1])
    if c == (2004, 29):
        return "CHR_SET", I(a[0], CHR["BACKREAD"], not a[1])
    if c == (2004, 4):
        return "KILL", I(a[0], a[1])
    if c == (2003, 18):
        return "ANIM", I(a[0], a[1], a[2], a[3])
    if c == (2004, 14):
        return "FACE", I(a[0], a[1])
    if c == (2004, 41):
        return "MOVE", I(a[0], a[1], a[2])
    if c == (2004, 9):
        return "STANDBY", I(a[0], a[1])
    if c == (2004, 8):
        return "SPEFFECT", I(a[0], a[1], 1)
    if c == (2004, 21):
        return "SPEFFECT", I(a[0], a[1], 0)
    if c == (2004, 13):
        return "NEST", I(a[0], a[1])
    if c == (2004, 17):
        return "AI_CMD", I(a[0], a[1], a[2])
    if c == (2003, 11):
        ctx.npc_name(a[3])
        return "BOSS_BAR", I(a[0], a[1], a[3])
    if c == (2003, 12):
        return "KILL_BOSS", I(a[0])
    if c == (2009, 6):
        return "BOSS_START", []
    if c == (2005, 3):
        return "OBJ_STATE", I(a[0], a[1])
    if c == (2005, 1):
        return "OBJ_DESTROY", I(a[0])
    if c == (2005, 2):
        return "OBJ_RESTORE", I(a[0])
    if c == (2005, 7):
        return "OBJ_END_ANIM", I(a[0], a[1])
    if c == (2005, 6):
        return "OBJ_ACT", I(a[0], a[2])
    if c == (2005, 14):
        return "OBJ_ACT", I(a[0], a[3])
    if c == (2005, 4):
        return "TREASURE", I(a[0], a[1])
    if c == (2006, 1):
        return "VFX", I(a[0], 0)
    if c == (2006, 2):
        return "VFX", I(a[0], 1)
    if c == (2006, 3):
        return "VFX_TEMP", I(a[0], a[1], a[3])
    if c == (2011, 1):
        return "MAPCOL", I(a[0], a[1])
    if c == (2005, 9):
        return "HAZARD", I(a[0], a[1], a[3]) + [W("f", a[5]), W("f", a[6])]
    if c == (2007, 1):
        ctx.text(a[0])
        return "DIALOG", I(a[0], a[3])
    if c == (2002, 3):
        return "CUTSCENE", I(a[0], 0, 0)
    if c == (2002, 4):
        return "CUTSCENE", I(a[0], a[2], 0)
    if c == (2010, 3):
        return "SOUND", I(a[0], a[1])
    if c == (2010, 2):
        return "SFX", I(a[0], a[1], a[2])
    if c == (2009, 0):
        ctx.text(10010300)
        ctx.text(10010301)
        return "LADDER", I(a[0], a[1], a[2])
    if c == (2009, 3):
        ctx.text(10010182)
        ctx.text(10010183)
        return "BONFIRE", I(a[0], a[1]) + [W("f", a[2]), W("f", a[3])]
    # dropped on purpose: network sync, prefetch, achievements, soapstone messages, multiplayer buffs,
    # humanity registration, Snuggly trades, mandatory NPC treasure (Oscar is cut)
    name = ctx.emevd.events[0].instructions[0].EMEDF.get(key, {}).get("alias", str(key))
    ctx.unsupported[name] = ctx.unsupported.get(name, 0) + 1
    return "NOP", []


def cmp_const(cmp: int, left: float, right: float) -> int:
    return int([left == right, left != right, left > right, left < right, left >= right, left <= right][cmp])


# --------------------------------------------------------------------------------------------- overrides

# event id -> (decision, note). Decisions: делаем / урезаем / выкидываем (instance replaced by an empty event).
DECISIONS = {
    0: ("делаем", "конструктор: костры, лестница, двери, туманы, вызовы событий; кат-сцена старта -> затемнение"),
    50: ("урезаем", "преконструктор: NG+ и ковенанты отпадают сами (константы), Оскар выкинут"),
    11810090: ("делаем", "туман CP1 у Оскара: «Пройти через белый свет» с двух сторон, анимация 7410"),
    11815090: ("выкидываем", "чёрный фантом по тенденции мира (1810900) — тенденции мира нет"),
    11815390: ("делаем", "вход в туман босса с балкона: демон встаёт под балкон (1812301), прыжок вниз"),
    11815393: ("делаем", "старт боя: игрок в зале/на балконе"),
    11815392: ("делаем", "включение ИИ демона после входа"),
    11810001: ("делаем", "смерть демона: флаг 16, KillBoss, туман и решётка открываются"),
    11815394: ("делаем", "полоска HP босса + музыка включаются"),
    11815395: ("делаем", "полоска HP + музыка выключаются при выходе/смерти"),
    11810310: ("делаем", "первая встреча: демон спрыгивает с потолка (анимация 9060), туман балкона загорается"),
    11810311: ("делаем", "бегство: при входе в зал решётка 1811115 открывается, большая дверь закрывается"),
    11810312: ("делаем", "за решёткой: решётка закрывается, точка возрождения -> костёр 2, сохранение"),
    11810313: ("делаем", "большая дверь после побега: «Заперто при помощи устройства»"),
    11815380: ("выкидываем", "туман Демона-отступника (второй визит) — после ворона демо заканчивается"),
    11815382: ("урезаем", "Демон-отступник: остаётся выключенным (до 11810000 не доходим)"),
    11810900: ("урезаем", "смерть Демона-отступника — недостижимо в срезе"),
    11815384: ("урезаем", "музыка ямы — недостижимо"),
    11815385: ("урезаем", "музыка ямы — недостижимо"),
    11810000: ("урезаем", "ворон: вместо кат-сцен 180125/100225 — экран «Конец демо»"),
    11810150: ("урезаем", "гнездо ворона (второй визит) — недостижимо"),
    11810100: ("делаем", "двери камер: ключ из ObjActParam, «Использовано: ключ»"),
    11810120: ("делаем", "дверь с внутренним замком: эффект 4150 рядом с ней"),
    11810110: ("делаем", "малая дверь церкви: Большой ключ паломника"),
    11810111: ("делаем", "большая дверь церкви открыта -> флаг 11810112"),
    11810211: ("урезаем", "железный шар: катится по прямой к стене 1811210 (без анимации объекта)"),
    11810200: ("делаем", "стена, которую ломает шар"),
    11810400: ("урезаем", "стартовое оружие/щит по классу: класс фиксирован (Воин) при куке"),
    11810450: ("выкидываем", "сообщения на полу (мыльный камень)"),
    11810220: ("урезаем", "провал пола в яму — только после ворона, недостижимо"),
    11810320: ("урезаем", "неуязвимость Демона-отступника до ворона"),
    11810300: ("делаем", "что роняет демон: убит до тумана CP1 -> молот 852000 (лот 2661), иначе ключ 2011 (лот 2660)"),
    11810850: ("урезаем", "чёрные рыцари второго визита: дроп по смерти; в срезе выключены"),
    11810350: ("делаем", "кто из врагов включён в первый визит (1810101..113), второй визит выключен"),
    11810600: ("выкидываем", "обмен с Снаглом"),
    11810641: ("выкидываем", "обмен с Снаглом"),
    11815110: ("выкидываем", "обмен с Снаглом"),
    11815150: ("делаем", "служебный флаг через 1 с после загрузки"),
    11810050: ("выкидываем", "ковенант Клинков тёмной луны (NG+)"),
    11810510: ("выкидываем", "Оскар: враждебность"),
    11810520: ("выкидываем", "Оскар: смерть/квест; эстус кладём на пол (Ash 91810001)"),
    11810530: ("выкидываем", "Оскар"),
    11810531: ("выкидываем", "Оскар (полый)"),
    11810532: ("выкидываем", "Оскар (полый)"),
    11815010: ("выкидываем", "Оскар: эффект"),
}
DROP_EVENTS = {k for k, (d, _) in DECISIONS.items() if d == "выкидываем"}


def build_code(ctx: Ctx, insts, index):
    for ii, inst in enumerate(insts):
        dec, note = DECISIONS.get(inst.eid, ("делаем", "?"))
        inst.decision, inst.note = dec, note
        if inst.eid in DROP_EVENTS:
            inst.code = []
            continue
        code = []
        for ins, args in inst.raw:
            key = (ins.category, ins.index)
            if key == (2000, 0):
                fmt = "@" + ins.struct_args_fmt
                sub = index[(args[1], args[0], struct.pack(fmt, *args)[8:])]
                if insts[sub].eid in DROP_EVENTS:
                    code.append((OP["NOP"], []))
                else:
                    code.append((OP["RUN"], [W("i", sub)]))
                continue
            name, words = conv(ctx, key, args, ii)
            code.append((OP[name], words))
        inst.code = code

    def patch(eid, fn):
        for inst in insts:
            if inst.eid == eid and inst.code:
                fn(inst)

    # -- opening cutscene 180101 (Oscar drops the corpse with the key) -> fade in at the post-cutscene start point
    def p0(inst):
        for i, (op, w) in enumerate(inst.code):
            if op == OP["CUTSCENE"] and w[0][1] == 180101:
                inst.code[i] = (OP["CUTSCENE"], [W("i", 180101), w[1], W("i", 900003)])
    patch(0, p0)

    # -- crow: cutscenes 180125 + 100225 (flight to Firelink) -> end of the demo; nothing after it runs
    def p11810000(inst):
        n = 0
        for i, (op, w) in enumerate(inst.code):
            if op == OP["CUTSCENE"]:
                inst.code[i] = (OP["END_DEMO"], [W("i", ctx.text(900002))]) if n == 0 else (OP["NOP"], [])
                n += 1
            elif op in (OP["RESPAWN"], OP["SAVE"]):
                inst.code[i] = (OP["NOP"], [])
    patch(11810000, p11810000)

    # -- iron ball: CreateHazard + object animation -> straight roll to the wall it breaks
    def p11810211(inst):
        for i, (op, w) in enumerate(inst.code):
            if op == OP["HAZARD"]:
                inst.code[i] = (OP["BALL"], [W("i", 1811215), W("i", 1811210), W("f", 6.0), W("f", 0.8)])
            elif op == OP["ANIM"] and w[0][1] == 1811215:
                inst.code[i] = (OP["NOP"], [])
    patch(11810211, p11810211)


def ash_events(ctx: Ctx, insts, treasures):
    """Ash-authored events (not in EMEVD). Return instance indices to RUN from the constructor."""
    I = lambda *vs: [W("i", int(v)) for v in vs]  # noqa: E731
    out = []

    def new(eid, code, note):
        inst = Instance(eid, 0, 0, b"", [])
        inst.code, inst.decision, inst.note = code, "добавлено", note
        insts.append(inst)
        out.append(len(insts) - 1)

    # 91810001: Estus Flask on the floor where Oscar lies (he is cut). Treasure index appended below.
    est = len(treasures)
    oscar = next(c for c in ctx.msb.characters if c.entity_id == 6023)
    p = rot_y([0.0, 0.0, 0.8], float(oscar.rotate.y))
    ctx.lots_used.add(1080)   # Oscar's gift: Undead Asylum F2 East Key
    treasures.append(dict(obj=0, lots=[-91810001, 1080], pos=[float(oscar.translate[i]) + p[i] for i in range(3)],
                          hidden=0, ash=dict(flag=51810990, items=[(4, 200, 1)])))
    new(91810001, [(OP["TREASURE_ADD"], I(est))], "эстус-фляга + ключ восточного крыла (лот Оскара 1080) на полу у места Оскара [придумал]")

    # 91810010: firebomb trap — the hollow at the top of the iron-ball stairs throws firebombs while the player
    # climbs (region of the ball trigger, 1812120). Not in EMEVD m18_01 [проверено: нет ShootBullet]; our trap.
    code = [
        (OP["C_DEAD"], I(1, 1810103, 0)),                                        # AND_1: thrower alive
        (OP["C_REGION"], I(1, 1, PLAYER, 1812120)),                          # AND_1: player on the stairs
        (OP["C_COND"], I(0, 1, 1)),                                         # MAIN await AND_1
        (OP["BOMB"], I(1810103, PLAYER, 3) + [W("f", 1.2)]),
        (OP["WAIT"], [W("f", 6.0)]),
        (OP["RET"], I(1)),
    ]
    new(91810010, code, "огненные бомбы: полый над лестницей шара кидает 3 бомбы, пока игрок на лестнице [придумал]")
    return out


# --------------------------------------------------------------------------------------------- tables

def build_entities(ctx: Ctx, insts):
    msb = ctx.msb
    ents = {}

    def put(eid, kind, shape, pos, yaw_deg, dim=(0, 0, 0), model=0, extra=0, extra2=0, name=None):
        if eid is None or eid <= 0:
            return
        ents[eid] = dict(id=eid, kind=kind, shape=shape, pos=mirror_pos(pos), yaw=mirror_yaw_deg(yaw_deg),
                         dim=[float(d) for d in dim], model=model, extra=extra, extra2=extra2, name=name)

    for r in msb.regions:
        sh = r.shape
        t = type(sh).__name__ if sh is not None else "PointShape"
        if t == "BoxShape":
            shape, dim = SHAPE["BOX"], (sh.width, sh.depth, sh.height)
        elif t == "SphereShape":
            shape, dim = SHAPE["SPHERE"], (sh.radius, 0, 0)
        elif t == "CylinderShape":
            shape, dim = SHAPE["CYLINDER"], (sh.radius, sh.height, 0)
        else:
            shape, dim = SHAPE["POINT"], (0, 0, 0)
        put(r.entity_id, EK["REGION"], shape, r.translate, r.rotate.y, dim)
    # object model geometry (fog walls 4.06 m from dummies 201/202; doors from FLVER bbox) — measured by hand
    # from obj/*.objbnd (docs/events.md §4) to keep the cooker light.
    OBJ_DIM = {
        8950: (-2.03, 2.03, 4.0), 8952: (-2.03, 2.03, 4.0), 8953: (-2.03, 2.03, 4.0),   # fog walls [height: оценка]
        8540: (-0.06, 1.44, 2.21), 8541: (-1.18, 1.18, 4.9), 8544: (-1.19, 1.18, 3.74),
        8542: (-0.8, 0.8, 2.27),                                                        # gate: extent along local z
        8500: (-0.52, 6.5, 0.47), 8501: (-0.52, 5.5, 0.47),                             # ladder: bottom z, top y, top z
        8520: (-1.7, 1.7, 3.65), 8530: (-0.56, 0.56, 1.12), 500: (-0.85, 0.84, 1.7),
    }
    spawn_of = {}
    for e in msb.get_events():
        if type(e).__name__ == "MSBSpawnPointEvent" and e.spawn_point_region is not None:
            r = e.spawn_point_region
            put(e.entity_id, EK["SPAWN"], SHAPE["POINT"], r.translate, r.rotate.y)
            spawn_of[e.entity_id] = r
    for o in msb.objects:
        m = model_num(o.model.name if o.model else None)
        put(o.entity_id, EK["OBJECT"], SHAPE["POINT"], o.translate, o.rotate.y, OBJ_DIM.get(m, (0, 0, 0)), m,
            name=o.name)
    for c in msb.characters:
        m = model_num(c.model.name if c.model else None)
        put(c.entity_id, EK["CHR"], SHAPE["POINT"], c.translate, c.rotate.y, (0, 0, 0), m, extra=c.character_id,
            extra2=c.ai_id, name=c.name)
    for c in msb.collisions:
        put(c.entity_id, EK["COLL"], SHAPE["POINT"], c.translate, 0.0, name=c.name)
    for e in msb.get_events():
        if type(e).__name__ == "MSBVFXEvent" and e.entity_id > 0:
            part = e.attached_part
            reg = e.attached_region
            src = reg if reg is not None else part
            put(e.entity_id, EK["VFX"], SHAPE["POINT"], src.translate, getattr(src.rotate, "y", 0.0),
                model=e.vfx_id, extra=part.entity_id if part is not None and part.entity_id > 0 else 0,
                name=part.name if part is not None else None)
    # bonfire objects: extra = spawn point (by the "_00/_01" suffix of 篝火復活イベント0N) [версия]
    for o in msb.objects:
        if o.entity_id in (1811960, 1811961):
            ents[o.entity_id]["extra"] = 1812960 + (o.entity_id - 1811960)
    return ents


def resolve_lot(ctx: Ctx, lot_id: int):
    IL = ctx.P("ItemLotParam")
    try:
        r = IL[lot_id]
    except Exception:
        return None
    cat_map = {0: 0, 0x10000000: 1, 0x20000000: 2, 0x40000000: 4}
    items = []
    for k in range(1, 9):
        iid = getattr(r, f"Item{k}ID")
        if iid <= 0:
            continue
        cat = cat_map.get(getattr(r, f"Item{k}Category"), 4)
        cnt = getattr(r, f"Item{k}Count") or 1
        if getattr(r, f"Item{k}ChancePoints") <= 0 and k > 1:
            continue
        items.append((cat, iid, cnt))
    return dict(flag=r.ItemFlag, items=items)


def build_treasures(ctx: Ctx, ents):
    out = []
    for e in ctx.msb.get_events():
        if type(e).__name__ != "MSBTreasureEvent" or e.treasure_part is None:
            continue
        p = e.treasure_part
        lots = [x for x in (e.item_lot_1, e.item_lot_2) if x and x > 0]
        # item sits on the corpse: dummy 90 of o0500 is at y 1.31 (the hand); we use the object origin + 0.3 m up
        out.append(dict(obj=p.entity_id if p.entity_id > 0 else 0, lots=lots + [0] * (2 - len(lots)),
                        pos=[float(p.translate[0]), float(p.translate[1]) + 0.3, float(p.translate[2])],
                        hidden=int(bool(e.is_hidden))))
        for x in lots:
            ctx.lots_used.add(x)
    return out


def build_objacts(ctx: Ctx):
    OA = ctx.P("ObjActParam")
    out = []
    for e in ctx.msb.get_events():
        if type(e).__name__ != "MSBObjActEvent":
            continue
        part = e.obj_act_part
        pid = e.obj_act_param_id if e.obj_act_param_id > 0 else model_num(part.name if part else None)
        try:
            r = OA[pid]
        except Exception:
            ctx.warn.append(f"ObjActParam {pid} missing for {e.obj_act_entity_id}")
            continue
        need, need_type = 0, 0
        if r.SuccessCondition1Type == 1 and r.SuccessConditionID1 > 0:       # item (goods) in inventory
            ctx.item_name(4, r.SuccessConditionID1)
            need, need_type = (4 << 28) | r.SuccessConditionID1, 1      # same key as LotItem.item
        elif r.SuccessCondition1Type == 2:                                    # special effect on the player
            need, need_type = r.SuccessConditionID1, 2
        ctx.text(r.PromptMessage)
        ctx.text(r.FailureMessage)
        out.append(dict(id=e.obj_act_entity_id, obj=part.entity_id if part else 0, prompt=r.PromptMessage,
                        fail=r.FailureMessage, need=need, need_type=need_type,
                        auto=r.FlagForAutomaticSuccess, done=e.obj_act_flag, param=pid))
    return out


# KillBoss(game_area) -> item lots, each gated by its own ItemFlag (event 11810300 sets/clears them).
# NpcParam 223200.ItemLotID1 = 22320000 does not exist in ItemLotParam [проверено]; the drops are lots 2660
# ("チュートリアル ミニデブデーモンドロップ通常": Big Pilot Key) and 2661 ("…特殊": Demon's Great Hammer) [проверено].
BOSS_DROPS = {1810800: (2660, 2661)}


def build_drops(ctx: Ctx):
    out = []
    for chr_id, lots in BOSS_DROPS.items():
        for lot in lots:
            out.append((chr_id, lot))
            ctx.lots_used.add(lot)
    return out


# --------------------------------------------------------------------------------------------- writer

def write(ctx: Ctx, insts, roots, ents, treasures, objacts, drops, out: Path):
    # lots (+ Ash-authored ones with negative ids)
    lots, lotitems = [], []
    lot_ids = sorted(ctx.lots_used)
    ash_lots = [(t["lots"][0], t["ash"]) for t in treasures if "ash" in t]
    for lid in lot_ids:
        r = resolve_lot(ctx, lid)
        if r is None:
            ctx.warn.append(f"ItemLot {lid} missing")
            continue
        first = len(lotitems)
        for cat, iid, cnt in r["items"]:
            lotitems.append((cat, iid, cnt, ctx.item_name(cat, iid)))
        lots.append((lid, r["flag"], first, len(r["items"])))
    for lid, a in ash_lots:
        first = len(lotitems)
        for cat, iid, cnt in a["items"]:
            lotitems.append((cat, iid, cnt, ctx.item_name(cat, iid)))
        lots.append((lid & 0xFFFFFFFF, a["flag"], first, len(a["items"])))
    ctx.text(900001)
    for k in ASH_TEXT:
        ctx.text(k)
    ctx.npc_name(2232)

    for e in ents.values():
        if e["kind"] == EK["COLL"] and e.get("name"):
            ctx.strings[(3 << 28) | e["id"]] = e["name"]
    strs = sorted(ctx.strings.items())
    pool = bytearray()
    strtab = []
    for k, s in strs:
        strtab.append((k, len(pool)))
        pool += s.encode("utf-8") + b"\0"
    while len(pool) % 4:
        pool += b"\0"
    str_index = {k: i for i, (k, _) in enumerate(strs)}

    code = []
    inst_rows = []
    for inst in insts:
        off = len(code)
        for op, words in inst.code:
            code.append(op | (len(words) << 16))
            for t, v in words:
                code.append(struct.unpack("<I", struct.pack("<f", float(v)))[0] if t == "f"
                            else int(v) & 0xFFFFFFFF)
        inst_rows.append(struct.pack("<IHBBII", inst.eid, inst.slot, inst.rest, 0, off, len(inst.code)))

    ent_rows = []
    for e in sorted(ents.values(), key=lambda x: x["id"]):
        sidx = str_index.get((3 << 28) | e["id"], 0xFFFF)
        ent_rows.append(struct.pack("<IBBH3ff3fIII", e["id"], e["kind"], e["shape"], sidx, *e["pos"], e["yaw"],
                                    *e["dim"], e["model"] & 0xFFFFFFFF, e["extra"] & 0xFFFFFFFF,
                                    e["extra2"] & 0xFFFFFFFF))
    tre_rows = [struct.pack("<I2I3fBBBBI", t["obj"], t["lots"][0] & 0xFFFFFFFF, t["lots"][1] & 0xFFFFFFFF,
                            *mirror_pos(t["pos"]), t["hidden"], 0, 0, 0, 0) for t in treasures]
    lot_rows = [struct.pack("<IIHH", lid & 0xFFFFFFFF, flag & 0xFFFFFFFF, first, n) for lid, flag, first, n in lots]
    li_rows = [struct.pack("<IHH", ((cat & 0xF) << 28) | iid, cnt, str_index.get(key, 0xFFFF))
               for cat, iid, cnt, key in lotitems]
    str_rows = [struct.pack("<II", k, off) for k, off in strtab]
    root_rows = [struct.pack("<I", r) for r in roots]
    oa_rows = [struct.pack("<IIIIIBBBBII", o["id"], o["obj"], o["prompt"] & 0xFFFFFFFF, o["fail"] & 0xFFFFFFFF,
                           o["need"], o["need_type"], 0, 0, 0, o["auto"] & 0xFFFFFFFF, o["done"] & 0xFFFFFFFF)
               for o in objacts]
    drop_rows = [struct.pack("<II", c, l) for c, l in drops]
    code_bytes = struct.pack(f"<{len(code)}I", *code)

    sections = [(len(inst_rows), b"".join(inst_rows)), (len(code), code_bytes), (len(ent_rows), b"".join(ent_rows)),
                (len(tre_rows), b"".join(tre_rows)), (len(lot_rows), b"".join(lot_rows)),
                (len(li_rows), b"".join(li_rows)), (len(str_rows), b"".join(str_rows)), (len(pool), bytes(pool)),
                (len(root_rows), b"".join(root_rows)), (len(oa_rows), b"".join(oa_rows)),
                (len(drop_rows), b"".join(drop_rows))]
    hdr_size = 8 + 8 * len(sections)
    hdr_size = (hdr_size + 15) & ~15
    body = bytearray()
    pairs = []
    for n, data in sections:
        pairs.append((n, hdr_size + len(body)))
        body += data
        while len(body) % 16:
            body += b"\0"
    hdr = bytearray(b"AEVT" + struct.pack("<I", 1))
    for n, o in pairs:
        hdr += struct.pack("<II", n, o)
    hdr += b"\0" * (hdr_size - len(hdr))
    blob = bytes(hdr + body)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(blob)
    return dict(bytes=len(blob), instances=len(insts), code_words=len(code), entities=len(ent_rows),
                treasures=len(tre_rows), lots=len(lot_rows), strings=len(str_rows), objacts=len(oa_rows),
                drops=len(drop_rows), crc32=f"{zlib.crc32(blob):08x}")


def gen_header(path: Path):
    lines = ["/* Generated by tools/cook_events.py --gen-header. Do not edit: change OPS there. */",
             "#ifndef ASH_EVOPS_H", "#define ASH_EVOPS_H", "", "enum {"]
    for i, (name, args) in enumerate(OPS):
        lines.append(f"    EVOP_{name} = {i},{' ' * max(1, 20 - len(name))}/* {args or '-'} */")
    lines += ["    EVOP_COUNT", "};", "", "enum {"]
    for i, n in enumerate(CHR_WHAT):
        lines.append(f"    EV_CHR_{n} = {i},")
    lines += ["};", "", "#ifdef EVOPS_NAMES", "static const char *const evop_names[EVOP_COUNT] = {"]
    lines += [f'    "{n}",' for n, _ in OPS]
    lines += ["};", "#endif", "", "#endif", ""]
    path.write_text("\n".join(lines))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--map", default="m18_01_00_00")
    ap.add_argument("--out", type=Path, default=Path.home() / "ash-assets/cooked/m18_01.ashevt")
    ap.add_argument("--class-type", type=int, default=CLASS_DEFAULT)
    ap.add_argument("--no-ash-extras", action="store_true", help="no Ash-authored events (Estus, firebombs)")
    ap.add_argument("--gen-header", type=Path)
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--doc", action="store_true", help="markdown table of every EMEVD event (docs/events.md)")
    a = ap.parse_args()
    if a.gen_header:
        gen_header(a.gen_header)
        print(f"wrote {a.gen_header} ({len(OPS)} opcodes)")
        return
    t0 = __import__("time").time()
    ctx = Ctx(a.map, a.class_type)
    insts, index, roots = flatten(ctx)
    build_code(ctx, insts, index)
    ents = build_entities(ctx, insts)
    treasures = build_treasures(ctx, ents)
    objacts = build_objacts(ctx)
    drops = build_drops(ctx)
    if not a.no_ash_extras:
        extra = ash_events(ctx, insts, treasures)
        # run them from the constructor: append RUNs to instance of event 0 (after its last line would be
        # unreachable because of early returns -> prepend instead; prepend shifts no skips: skips are relative)
        c0 = insts[roots[1]]
        c0.code = [(OP["RUN"], [W("i", i)]) for i in extra] + c0.code
    if a.doc:
        called = {}
        for inst in insts:
            called.setdefault(inst.eid, []).append(inst.slot)
        print("| Событие | Рест | Слоты | Решение | Что это / как у нас |")
        print("|---|---|---|---|---|")
        for eid, e in sorted(ctx.emevd.events.items()):
            if eid in (0, 50) or True:
                dec, note = DECISIONS.get(eid, ("—", ""))
                slots = called.get(eid)
                if not slots:
                    dec, note = "не вызывается", note or "нет RunEvent из 0/50 в m18_01"
                rest = "Restart" if e.on_rest_behavior.value else "Continue"
                print(f"| {eid} | {rest} | {len(slots) if slots else 0} | {dec} | {note} |")
        for inst in insts:
            if inst.eid >= 91810000:
                print(f"| {inst.eid} | Continue | 1 | {inst.decision} | {inst.note} |")
        return
    if a.list:
        for i, inst in enumerate(insts):
            print(f"{i:3d} {inst.eid:9d}/{inst.slot:<2d} rest={'R' if inst.rest else 'C'} n={len(inst.code):3d} "
                  f"{inst.decision:10s} {inst.note}")
    st = write(ctx, insts, roots, ents, treasures, objacts, drops, a.out)
    st["seconds"] = round(__import__("time").time() - t0, 1)
    st["unsupported_nop"] = ctx.unsupported
    st["warnings"] = ctx.warn
    st["decisions"] = {str(i.eid) + "/" + str(i.slot): i.decision for i in insts}
    a.out.with_suffix(".json").write_text(json.dumps(st, ensure_ascii=False, indent=1))
    print(json.dumps({k: v for k, v in st.items() if k != "decisions"}, ensure_ascii=False))


if __name__ == "__main__":
    main()
