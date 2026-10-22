#!/usr/bin/env python3
"""Decode crash.txt of "Ash: Dark Souls" (src/diag): addresses -> function+offset (and file:line).

  python3 tools/crash_decode.py crash.txt [--sym dist/debug/ash_ds.sym] [--elf dist/debug/ash_ds.elf]

The EBOOT is a static ELF loaded at its link address (0x08804000), so crash addresses are link addresses.
ash_ds.sym = `psp-nm -n --defined-only` of the unstripped ELF (local statics included); the ELF adds file:line via
psp-addr2line (from $PSPDEV/bin, optional). make-dist.sh puts both into dist/debug/ (not on the memory stick).
"""
import argparse, bisect, os, re, shutil, subprocess, sys
from pathlib import Path


def load_syms(path):
    syms = []
    for line in Path(path).read_text(errors="replace").splitlines():
        p = line.split()
        if len(p) == 3 and p[1].lower() in "tw":
            syms.append((int(p[0], 16), p[2]))
    syms.sort()
    return syms


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("crash")
    here = Path(__file__).resolve().parent.parent
    ap.add_argument("--sym", default=None)
    ap.add_argument("--elf", default=None)
    a = ap.parse_args()
    crash = Path(a.crash)
    near = crash.parent
    cands = [near / "debug", near, here / "dist/debug", here / "build/src/game/ds"]
    sym = a.sym or next((str(d / "ash_ds.sym") for d in cands if (d / "ash_ds.sym").exists()), None)
    elf = a.elf or next((str(d / "ash_ds.elf") for d in cands if (d / "ash_ds.elf").exists()), None)
    text = crash.read_text(errors="replace")
    syms = load_syms(sym) if sym else []
    if not syms and elf:
        nm = shutil.which("psp-nm") or os.path.expandvars("$PSPDEV/bin/psp-nm")
        out = subprocess.run([nm, "-n", "--defined-only", elf], capture_output=True, text=True).stdout
        syms = sorted((int(p[0], 16), p[2]) for p in (l.split() for l in out.splitlines())
                      if len(p) == 3 and p[1].lower() in "tw")
    if not syms:
        sys.exit("no symbols: pass --sym ash_ds.sym or --elf ash_ds.elf")
    addrs = [s[0] for s in syms]
    a2l = shutil.which("psp-addr2line") or os.path.expandvars("$PSPDEV/bin/psp-addr2line")

    def where(x):
        i = bisect.bisect_right(addrs, x) - 1
        f = f"{syms[i][1]}+0x{x - syms[i][0]:x}" if i >= 0 else "?"
        if elf and os.path.exists(a2l):
            r = subprocess.run([a2l, "-e", elf, hex(x)], capture_output=True, text=True).stdout.strip()
            if r and not r.startswith("??"):
                f += "  " + r.split("/src/")[-1]
        return f

    head = text.split("--- last log lines ---")[0]
    for k in ("EPC", "ra"):
        m = re.search(rf"\b{k}=0x([0-9a-f]+)", head)
        if m:
            print(f"{k:5} 0x{m.group(1)}  {where(int(m.group(1), 16))}")
    m = re.search(r"^trace:(.*)$", head, re.M)
    if m:
        print("call chain (heuristic, innermost first):")
        for t in re.findall(r"0x([0-9a-f]+)", m.group(1)):
            print(f"   0x{t}  {where(int(t, 16))}")


if __name__ == "__main__":
    main()
