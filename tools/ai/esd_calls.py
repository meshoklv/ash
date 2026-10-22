#!/usr/bin/env python3
"""Count which EzState (ESD) test functions and commands a chr ESD really uses.

    tools/.venv/bin/python tools/ai/esd_calls.py chr/c0000.esd.dcx [chr/enemyCommon.esd.dcx chr/m18.chresdbnd.dcx]

Paths are relative to $ASH_DSR (default ~/ash-assets/dsr). A .chresdbnd is a binder of ESDs (all entries scanned).

EZL bytecode is scanned with our own stack walker (same opcodes as soulstruct's ezl_parser): a call `0x84+N` pops N
args and then the function id (pushed first). Counts are per call site, not per execution. Names come from soulstruct's
function table, which is shared by all FromSoft games — a name is a hint, the id is the fact (combat-ai notes §4.2).
"""
from __future__ import annotations

import logging
import os
import struct
import sys
from collections import Counter
from pathlib import Path

logging.disable(logging.WARNING)
from soulstruct.base.ezstate.esd.esd_type import ESDType  # noqa: E402
from soulstruct.base.ezstate.esd.functions import COMMANDS, TEST_FUNCTIONS  # noqa: E402
from soulstruct.containers import Binder  # noqa: E402
from soulstruct.darksouls1r.ezstate.esd import ChrESD  # noqa: E402

DSR = Path(os.environ.get("ASH_DSR", Path.home() / "ash-assets/dsr"))


def scan_ezl(b: bytes, calls: Counter, arity: dict):
    st: list = []
    i = 0
    while i < len(b):
        c = b[i]
        if c <= 0x7F:
            st.append(c - 64); i += 1
        elif c == 0x80:
            st.append(struct.unpack_from("<f", b, i + 1)[0]); i += 5
        elif c == 0x81:
            st.append(struct.unpack_from("<d", b, i + 1)[0]); i += 9
        elif c == 0x82:
            st.append(struct.unpack_from("<i", b, i + 1)[0]); i += 5
        elif 0x84 <= c <= 0x8A:
            n = c - 0x84
            args = st[len(st) - n:] if n else []
            del st[len(st) - n:]
            fid = st.pop() if st else None
            if isinstance(fid, (int, float)):
                fid = int(fid)
                calls[fid] += 1
                arity.setdefault(fid, set()).add(n)
            st.append(("call", fid))
            i += 1
        elif 0x8C <= c <= 0x99 and c != 0x8D:
            if len(st) >= 2:
                st.pop(); st.pop()
            st.append("expr"); i += 1
        elif c == 0x8D:
            i += 1  # unary
        elif c == 0xA1:
            i += 1
        elif c == 0xA5:
            j = i + 1
            while b[j:j + 2] != b"\0\0":
                j += 2
            st.append("str"); i = j + 2
        elif c == 0xA6 or c == 0xB7 or c == 0xB8:
            i += 1
        elif 0xA7 <= c <= 0xAE:
            i += 1
        elif 0xAF <= c <= 0xB6:
            st.append("reg"); i += 1
        elif c == 0xB9:
            st.append("state_identity"); i += 1  # "is this the state" / current state id
        elif c == 0xBA:
            st.append("state_identity"); i += 1
        else:
            st.append(f"op{c:02x}"); i += 1
            calls[("unknown_op", c)] += 1


def walk_conditions(conds, calls, arity, cmds):
    for cnd in conds:
        scan_ezl(bytes(cnd.test_ezl), calls, arity)
        for cm in cnd.pass_commands:
            cmds[(cm.bank, cm.index)] += 1
            for a in cm.args:
                scan_ezl(bytes(a), calls, arity)
        walk_conditions(cnd.subconditions, calls, arity, cmds)


def scan_esd(esd, calls, arity, cmds) -> int:
    n = 0
    for sm in esd.state_machines.values():
        for st in sm.values():
            n += 1
            for lst in (st.enter_commands, st.exit_commands, st.ongoing_commands):
                for cm in lst:
                    cmds[(cm.bank, cm.index)] += 1
                    for a in cm.args:
                        scan_ezl(bytes(a), calls, arity)
            walk_conditions(st.conditions, calls, arity, cmds)
    return n


def report(title, calls, arity, cmds, n_states, n_sm):
    print(f"## {title}: {n_sm} state machines, {n_states} states")
    print(f"### test functions: {sum(v for k, v in calls.items() if isinstance(k, int))} call sites, "
          f"{sum(1 for k in calls if isinstance(k, int))} distinct")
    for fid, n in sorted(((k, v) for k, v in calls.items() if isinstance(k, int)), key=lambda x: (-x[1], x[0])):
        nm = TEST_FUNCTIONS[ESDType.CHR].get(fid, ("?",))[0]
        print(f"  {fid:4d} {n:5d}  args={sorted(arity[fid])}  {nm}")
    unk = {k: v for k, v in calls.items() if not isinstance(k, int)}
    if unk:
        print(f"  unknown opcodes: {unk}")
    print(f"### commands: {sum(cmds.values())} sites, {len(cmds)} distinct")
    for (bk, ix), n in sorted(cmds.items(), key=lambda x: (-x[1], x[0])):
        nm = COMMANDS[ESDType.CHR].get(bk, {}).get(ix, ("?",))[0]
        print(f"  [{bk}][{ix:3d}] {n:5d}  {nm}")
    print()


def main():
    for rel in sys.argv[1:] or ["chr/c0000.esd.dcx"]:
        p = DSR / rel
        calls, arity, cmds = Counter(), {}, Counter()
        n_states = n_sm = 0
        if "chresdbnd" in p.name:
            for e in Binder.from_path(p).entries:
                esd = ChrESD.from_bytes(e.data)
                n_sm += len(esd.state_machines)
                n_states += scan_esd(esd, calls, arity, cmds)
        else:
            esd = ChrESD.from_path(p)
            n_sm = len(esd.state_machines)
            n_states = scan_esd(esd, calls, arity, cmds)
        report(rel, calls, arity, cmds, n_states, n_sm)


if __name__ == "__main__":
    main()
