#!/usr/bin/env python3
"""Minimal unified-diff applier for one file (no `patch` binary in the dev container).

    python3 tools/perf/apply_patch.py <in.c> <diff> <out.c>
Only the hunks of the file in the diff whose path ends with <in.c>'s name are used. Hunks must apply with exact context (offsets allowed). Used by src/perf/CMakeLists.txt to build the A/B EBOOT
from a patched copy of src/game/asy_main.c (src/perf/asy_fast.patch); the original file is never touched.
"""
import re
import sys


def main():
    src, diff, dst = sys.argv[1:4]
    lines = open(src, encoding="utf-8").read().split("\n")
    hunks, cur, mine = [], None, False
    base = src.replace("\\", "/").split("/")[-1]
    for ln in open(diff, encoding="utf-8").read().split("\n"):
        if ln.startswith("+++ "):
            mine = ln[4:].strip().split("/")[-1] == base
            cur = None
            continue
        if not mine:
            continue
        m = re.match(r"^@@ -(\d+)(?:,\d+)? \+\d+(?:,\d+)? @@", ln)
        if m:
            cur = {"start": int(m.group(1)) - 1, "old": [], "new": []}
            hunks.append(cur)
        elif cur is not None and ln[:1] in (" ", "-", "+"):
            if ln[0] in " -":
                cur["old"].append(ln[1:])
            if ln[0] in " +":
                cur["new"].append(ln[1:])
        elif cur is not None and ln == "":
            pass
    shift = 0
    for h in hunks:
        old, at = h["old"], h["start"] + shift
        pos = None
        for d in range(0, 200):
            for p in (at + d, at - d):
                if 0 <= p and lines[p:p + len(old)] == old:
                    pos = p
                    break
            if pos is not None:
                break
        if pos is None:
            raise SystemExit(f"hunk at line {h['start'] + 1} does not apply")
        lines[pos:pos + len(old)] = h["new"]
        shift += len(h["new"]) - len(old)
    open(dst, "w", encoding="utf-8").write("\n".join(lines))


if __name__ == "__main__":
    main()
