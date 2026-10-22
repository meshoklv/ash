#!/usr/bin/env python3
"""Extract DSR AI Lua scripts, decompile them, and list the engine API a given battle goal really needs.

    tools/.venv/bin/python tools/ai/extract_ai.py                       # aiCommon + m18_01, root goal 250000
    tools/.venv/bin/python tools/ai/extract_ai.py --goal 223200 --map m18_01_00_00

Writes (derived FromSoftware assets, never in git) to ~/ash-assets/ai/<bnd>/:
  <name>.luac   original x64 bytecode (Lua 5.0, size_t 8, Instruction 4, double)
  <name>.lua    DSLuaDecompiler output (soulstruct's bundled .NET exe run under mono), Shift-JIS -> UTF-8
  <name>.lst    `luac -l` listing from our host Lua 5.0.3 (tools/ai/build_lua50.sh) — ground truth when the
                decompiler is wrong (it emits bogus `X = Y` lines for closure upvalues, see the ai-facts design notes)
and ~/ash-assets/ai/api_<goal>.txt — the static closure of the goal: Lua goals, common functions, ai:/goal: methods,
native goal ids and constants. The closure is computed on the bytecode listing (function level), not on the source.

Tools: mono from ~/opt/mono (micromamba, conda-forge `mono`), Lua 5.0.3 from ~/opt/lua50.
"""
from __future__ import annotations

import argparse
import logging
import os
import re
import subprocess
import tempfile
from collections import Counter, defaultdict
from pathlib import Path

logging.disable(logging.WARNING)
from soulstruct.containers import Binder  # noqa: E402

DSR = Path(os.environ.get("ASH_DSR", Path.home() / "ash-assets/dsr"))
OUT = Path(os.environ.get("ASH_AI_OUT", Path.home() / "ash-assets/ai"))
MONO = Path.home() / "opt/mono/bin/mono"
LUAC = Path.home() / "opt/lua50/bin/luac"
LUA = Path.home() / "opt/lua50/bin/lua"


def decompiler_dir() -> Path:
    from soulstruct.utilities.files import SOULSTRUCT_PATH
    return Path(SOULSTRUCT_PATH("base/ai/lua/x64"))


# ------------------------------------------------------------------ extraction


def extract(bnd: Path, out: Path, decompile: bool) -> list[Path]:
    out.mkdir(parents=True, exist_ok=True)
    files = []
    order = []
    dec = decompiler_dir()
    for e in Binder.from_path(bnd).entries:
        name = e.name.replace("\\", "/").rsplit("/", 1)[-1]
        if not name.endswith(".lua"):
            (out / name).write_bytes(e.data)
            continue
        stem = name[:-4]
        order.append(stem)
        luac = out / f"{stem}.luac"
        luac.write_bytes(e.data)
        files.append(luac)
        lst = subprocess.run([str(LUAC), "-l", "-p", str(luac)], capture_output=True)
        (out / f"{stem}.lst").write_bytes(lst.stdout + lst.stderr)
        if decompile:
            with tempfile.TemporaryDirectory() as td:
                t = Path(td) / "temp"
                t.write_bytes(e.data)
                r = subprocess.run([str(MONO), str(dec / "DSLuaDecompiler.exe"), "temp"], cwd=td,
                                   capture_output=True, timeout=120)
                src = Path(td) / "temp.dec.lua"
                if src.exists():
                    text = src.read_bytes().decode("cp932", "replace").lstrip("﻿")
                    hdr = (f"-- {name}: decompiled by DSLuaDecompiler (mono) from DSR {bnd.name}. FromSoftware data,\n"
                           f"-- never commit. Lines `X = Y` outside functions are decompiler artefacts (closure upvalues);\n"
                           f"-- trust {stem}.lst where they disagree.\n")
                    (out / f"{stem}.lua").write_text(hdr + text, encoding="utf-8")
                else:
                    (out / f"{stem}.lua.err").write_bytes(r.stdout + r.stderr)
    # binder entry order = the order we load in (src/ai/aitest.c reads it); goal_list (1000) comes first
    (out / "load_order.txt").write_text("\n".join(order) + "\n")
    return files


# ------------------------------------------------------------------ bytecode listing -> function graph

HDR = re.compile(r"^(main|function) <(.+?):(\d+)> \(\d+ instructions?, \d+ bytes at (0x[0-9a-f]+)\)")
INS = re.compile(r"^\s+\d+\s+\[\d+\]\s+(\w+)\s+([-\d ]+?)\s*(?:;\s*(.*))?$")


class Fn:
    def __init__(self, addr, file, line):
        self.addr, self.file, self.line = addr, file, line
        self.name = None          # global it is stored in (top level), or parent's name
        self.gets: Counter = Counter()
        self.sets: set = set()
        self.selfs: Counter = Counter()    # method name -> count (receiver resolved below)
        self.self_recv: Counter = Counter()  # (reg, name)
        self.closures: list[str] = []
        self.numbers: Counter = Counter()
        self.nparams = 0


def parse_listing(text: str, file: str) -> tuple[list[Fn], list[tuple[str, str, str]]]:
    """-> functions, goal registrations [(REGISTER_x, GOAL_const, "Name")]."""
    fns: list[Fn] = []
    regs = []
    cur = None
    pending_closure = {}
    names: dict[str, str] = {}
    for line in text.splitlines():
        m = HDR.match(line)
        if m:
            cur = Fn(m.group(4), file, int(m.group(3)))
            fns.append(cur)
            pending_closure = {}
            continue
        if cur is None:
            continue
        if re.match(r"^\d+\+? params?", line):
            cur.nparams = int(re.match(r"^(\d+)", line).group(1))
            continue
        m = INS.match(line)
        if not m:
            continue
        op, args, com = m.group(1), m.group(2).split(), (m.group(3) or "").strip()
        if op == "GETGLOBAL":
            cur.gets[com] += 1
        elif op == "SETGLOBAL":
            cur.sets.add(com)
            a = args[0]
            if a in pending_closure:
                names.setdefault(pending_closure.pop(a), com)
        elif op == "CLOSURE":
            cur.closures.append(com)
            pending_closure[args[0]] = com
        elif op == "SELF":
            nm = com.strip('"')
            cur.selfs[nm] += 1
            cur.self_recv[(int(args[1]), nm)] += 1
        elif op == "LOADK":
            try:
                cur.numbers[float(com)] += 1
            except ValueError:
                pass
    for f in fns:
        f.name = names.get(f.addr)
    # registrations: scan main text linearly
    lines = [INS.match(x) for x in text.splitlines()]
    seq = [(m.group(1), (m.group(3) or "").strip()) for m in lines if m]
    for i in range(len(seq) - 2):
        (o1, c1), (o2, c2), (o3, c3) = seq[i:i + 3]
        if o1 == "GETGLOBAL" and c1.startswith("REGISTER_") and o2 == "GETGLOBAL" and c2.startswith("GOAL_"):
            regs.append((c1, c2, c3.strip('"') if o3 == "LOADK" else ""))
    return fns, regs


def lua_globals(script: Path) -> dict[str, float]:
    code = ("for k,v in pairs(_G) do if type(v)=='number' then print(k..'\\t'..string.format('%.17g',v)) end end")
    r = subprocess.run([str(LUA), "-e", f"dofile('{script}')", "-e", code], capture_output=True, text=True)
    out = {}
    for ln in r.stdout.splitlines():
        k, v = ln.split("\t")
        out[k] = float(v)
    return out


def closure(goal_id: int, dirs: list[Path], extra_fns: list[str], extra_goals: list[str]) -> str:
    fns: list[Fn] = []
    regs = []
    for d in dirs:
        for lst in sorted(d.glob("*.lst")):
            f, r = parse_listing(lst.read_text("latin1"), f"{d.name}/{lst.stem}")
            fns += f
            regs += [(a, b, c, f"{d.name}/{lst.stem}") for a, b, c in r]
    by_addr = {f.addr: f for f in fns}
    # children inherit the top-level name of their parent
    for f in fns:
        for c in f.closures:
            ch = by_addr.get(c)
            if ch is not None and ch.name is None and f.name:
                ch.name = f.name
    by_name: dict[str, list[Fn]] = defaultdict(list)
    for f in fns:
        if f.name:
            by_name[f.name].append(f)
    consts = {}
    common = dirs[0]
    for nm in ("ai_define", "goal_list", "logic_list", "event_list", "goal_list_dlc"):
        p = common / f"{nm}.luac"
        if p.exists():
            consts.update(lua_globals(p))
    goal_by_id = {int(v): k for k, v in consts.items() if k.startswith("GOAL_")}
    lua_goal = {}  # GOAL_const -> (Name, file, flags)
    for reg, gc, nm, file in regs:
        g = lua_goal.setdefault(gc, dict(name=None, file=file, flags=set()))
        if reg == "REGISTER_GOAL":
            g["name"] = nm
        else:
            g["flags"].add(reg)
    # BFS over function names
    roots = []
    gname = goal_by_id.get(goal_id)
    todo_goals = [gname] if gname else []
    seen_goals, seen_fns = set(), set()
    native_goals = Counter()
    methods = defaultdict(Counter)
    const_used = Counter()
    globals_used = Counter()
    fn_queue = []

    def add_goal(gc):
        if gc in seen_goals:
            return
        seen_goals.add(gc)
        g = lua_goal.get(gc)
        fns_ = [g["name"] + suf for suf in ("_Activate", "_Update", "_Terminate", "_Interupt")] if g and g["name"] else []
        if any(f in by_name for f in fns_):
            fn_queue.extend(fns_)
        else:
            native_goals[gc] += 1  # no Lua body: either REGISTER_GOAL by name only (common_func.lua) or unknown

    for g in todo_goals:
        add_goal(g)
    for g in extra_goals:
        add_goal(g)
    fn_queue.extend(extra_fns)
    while fn_queue:
        n = fn_queue.pop()
        if n in seen_fns:
            continue
        seen_fns.add(n)
        for f in by_name.get(n, []):
            for (reg, m), c in f.self_recv.items():
                recv = "ai" if reg == 0 else "goal" if reg == 1 else f"r{reg}"
                methods[recv][m] += c
            for g, c in f.gets.items():
                if g.startswith("GOAL_") and not g.startswith("GOAL_RESULT_"):
                    add_goal(g)
                    const_used[g] += c
                elif g in by_name:
                    globals_used[g] += c
                    fn_queue.append(g)
                elif g in consts:
                    const_used[g] += c
                else:
                    globals_used[g] += c
    L = [f"# API closure of battle goal {goal_id} ({gname}) + {extra_fns} + {extra_goals}",
         "# static, function level: every function reachable by name, so it over-approximates what one run calls", ""]
    L.append(f"## Lua goals executed by the VM ({sum(1 for g in seen_goals if g not in native_goals)})")
    for g in sorted(seen_goals - set(native_goals)):
        info = lua_goal[g]
        L.append(f"  {g} = {int(consts.get(g, -1))}  {info['name']}  [{info['file']}]  {' '.join(sorted(info['flags']))}")
    L.append(f"\n## Native goals (no Lua body in the bnds scanned) ({len(native_goals)})")
    for g in sorted(native_goals, key=lambda x: consts.get(x, 0)):
        info = lua_goal.get(g)
        how = f"REGISTER_GOAL by name '{info['name']}' in {info['file']}, no functions" if info else "never registered"
        L.append(f"  {g} = {int(consts.get(g, -1))}  ({how})")
    L.append(f"\n## Lua functions reached ({len(seen_fns & set(by_name))})")
    for n in sorted(seen_fns & set(by_name)):
        L.append(f"  {n}  [{by_name[n][0].file}]")
    L.append("\n## Methods (receiver from SELF register: r0 = ai, r1 = goal in (ai, goal, ...) functions)")
    for recv in sorted(methods):
        L.append(f"  {recv}: " + ", ".join(f"{m}({c})" for m, c in sorted(methods[recv].items(), key=lambda x: -x[1])))
    L.append("\n## Constants used")
    for g, c in sorted(const_used.items()):
        L.append(f"  {g} = {consts[g]:g}  ({c})" if g in consts else f"  {g}  ({c})")
    L.append("\n## Other globals read (not functions in the scanned bnds, not ai_define/goal_list constants)")
    for g, c in sorted(globals_used.items()):
        if g not in by_name:
            L.append(f"  {g}  ({c})")
    return "\n".join(L) + "\n"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--map", default="m18_01_00_00")
    ap.add_argument("--goal", type=int, default=250000)
    ap.add_argument("--no-decompile", action="store_true")
    ap.add_argument("--logic-fn", default="common10000_Logic,common10000_Interupt",
                    help="logic functions to include (NpcThinkParam.LogicID 10000 -> 010000_logic.lua)")
    ap.add_argument("--top-goal", action="store_true", default=True)
    ap.add_argument("--skip-extract", action="store_true")
    a = ap.parse_args()
    dirs = [OUT / "aiCommon", OUT / a.map]
    if not a.skip_extract:
        for d, bnd in zip(dirs, [DSR / "script/aiCommon.luabnd.dcx", DSR / f"script/{a.map}.luabnd.dcx"]):
            fs = extract(bnd, d, not a.no_decompile)
            nd = len(list(d.glob("*.lua")))
            print(f"{bnd.name}: {len(fs)} scripts -> {d} ({nd} decompiled)")
    txt = closure(a.goal, dirs, a.logic_fn.split(",") if a.logic_fn else [],
                  ["GOAL_COMMON_TopGoal"] if a.top_goal else [])
    p = OUT / f"api_{a.goal}.txt"
    p.write_text(txt)
    print(txt)


if __name__ == "__main__":
    main()
