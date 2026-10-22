#!/usr/bin/env python3
"""Repack DSR Lua 5.0 bytecode (x64: size_t 8, double) for a 32-bit VM (PSP Allegrex: size_t 4, double or float).

    tools/.venv/bin/python tools/ai/luac_repack.py IN.luac OUT.luac [--number float|double] [--strip]
    tools/.venv/bin/python tools/ai/luac_repack.py --scan DIR...      # number-literal scan: does float32 suffice?

No decompilation and no recompilation: the instruction stream is copied bit for bit (Instruction is 4 bytes in
both), only the header, string lengths (size_t) and number constants change. This is why the original scripts run
exactly — the decompiler output is not recompilable (the ai-facts design notes).

Format (lundump.c of Lua 5.0.3): header "\\x1bLua" 0x50 endian sizeof(int) sizeof(size_t) sizeof(Instruction)
SIZE_OP SIZE_A SIZE_B SIZE_C sizeof(lua_Number) TEST_NUMBER(3.14159265358979323846E7);
function: string source, int linedefined, u8 nups, numparams, is_vararg, maxstack, int[] lineinfo,
{string, int, int}[] locvars, string[] upvalues, {u8 type; number | string | nil}[] k, function[] p,
Instruction[] code. string = size_t len (0 = NULL, else len includes the trailing NUL).
--strip drops lineinfo/locvars/upvalue names (as `luac -s`): smaller and faster to load, but errors lose line numbers.
"""
from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

TEST_NUMBER = 3.14159265358979323846e7


class Reader:
    def __init__(self, b: bytes):
        self.b, self.i = b, 0

    def take(self, n):
        v = self.b[self.i:self.i + n]
        if len(v) != n:
            raise ValueError("truncated chunk")
        self.i += n
        return v

    def u8(self):
        return self.take(1)[0]

    def int(self):
        return struct.unpack("<i", self.take(4))[0]

    def size(self):
        return struct.unpack("<Q", self.take(8))[0]

    def num(self):
        return struct.unpack("<d", self.take(8))[0]

    def string(self):
        n = self.size()
        return None if n == 0 else self.take(n)


def read_function(r: Reader) -> dict:
    f = dict(source=r.string(), line=r.int(), nups=r.u8(), nparams=r.u8(), vararg=r.u8(), maxstack=r.u8())
    f["lines"] = [r.int() for _ in range(r.int())]
    f["locs"] = [(r.string(), r.int(), r.int()) for _ in range(r.int())]
    f["upv"] = [r.string() for _ in range(r.int())]
    k = []
    for _ in range(r.int()):
        t = r.u8()
        if t == 3:
            k.append(("n", r.num()))
        elif t == 4:
            k.append(("s", r.string()))
        elif t == 0:
            k.append(("nil", None))
        else:
            raise ValueError(f"bad constant type {t}")
    f["k"] = k
    f["p"] = [read_function(r) for _ in range(r.int())]
    f["code"] = r.take(4 * r.int())
    return f


def read_chunk(b: bytes) -> dict:
    r = Reader(b)
    hdr = r.take(13)
    want = b"\x1bLua\x50\x01\x04\x08\x04\x06\x08\x09\x09"
    if hdr != want:
        raise ValueError(f"unexpected header {hdr.hex()} (want {want.hex()})")
    if r.u8() != 8 or abs(r.num() - TEST_NUMBER) > 1:
        raise ValueError("number format is not double")
    f = read_function(r)
    if r.i != len(b):
        raise ValueError(f"{len(b) - r.i} trailing bytes")
    return f


def write_chunk(f: dict, number: str, strip: bool) -> bytes:
    out = bytearray()
    nfmt, nsize = ("<f", 4) if number == "float" else ("<d", 8)

    def s(x):
        if x is None:
            out.extend(struct.pack("<I", 0))
        else:
            out.extend(struct.pack("<I", len(x))); out.extend(x)

    def fn(f, top):
        s(f["source"] if (top or not strip) else None)
        out.extend(struct.pack("<i4B", f["line"], f["nups"], f["nparams"], f["vararg"], f["maxstack"]))
        lines = [] if strip else f["lines"]
        out.extend(struct.pack("<i", len(lines))); out.extend(struct.pack(f"<{len(lines)}i", *lines))
        locs = [] if strip else f["locs"]
        out.extend(struct.pack("<i", len(locs)))
        for n, a, b in locs:
            s(n); out.extend(struct.pack("<ii", a, b))
        upv = [] if strip else f["upv"]
        out.extend(struct.pack("<i", len(upv)))
        for n in upv:
            s(n)
        out.extend(struct.pack("<i", len(f["k"])))
        for t, v in f["k"]:
            if t == "n":
                out.append(3); out.extend(struct.pack(nfmt, v))
            elif t == "s":
                out.append(4); s(v)
            else:
                out.append(0)
        out.extend(struct.pack("<i", len(f["p"])))
        for p in f["p"]:
            fn(p, False)
        out.extend(struct.pack("<i", len(f["code"]) // 4)); out.extend(f["code"])

    out.extend(b"\x1bLua\x50\x01\x04\x04\x04\x06\x08\x09\x09")
    out.append(nsize)
    out.extend(struct.pack(nfmt, TEST_NUMBER))
    fn(f, True)
    return bytes(out)


def write_chunk64(f: dict) -> bytes:
    """Identity writer (x64 layout) — used only to prove the reader is lossless."""
    out = bytearray()

    def s(x):
        out.extend(struct.pack("<Q", 0 if x is None else len(x)))
        if x is not None:
            out.extend(x)

    def fn(f):
        s(f["source"])
        out.extend(struct.pack("<i4B", f["line"], f["nups"], f["nparams"], f["vararg"], f["maxstack"]))
        out.extend(struct.pack("<i", len(f["lines"]))); out.extend(struct.pack(f"<{len(f['lines'])}i", *f["lines"]))
        out.extend(struct.pack("<i", len(f["locs"])))
        for n, a, b in f["locs"]:
            s(n); out.extend(struct.pack("<ii", a, b))
        out.extend(struct.pack("<i", len(f["upv"])))
        for n in f["upv"]:
            s(n)
        out.extend(struct.pack("<i", len(f["k"])))
        for t, v in f["k"]:
            if t == "n":
                out.append(3); out.extend(struct.pack("<d", v))
            elif t == "s":
                out.append(4); s(v)
            else:
                out.append(0)
        out.extend(struct.pack("<i", len(f["p"])))
        for p in f["p"]:
            fn(p)
        out.extend(struct.pack("<i", len(f["code"]) // 4)); out.extend(f["code"])

    out.extend(b"\x1bLua\x50\x01\x04\x08\x04\x06\x08\x09\x09\x08")
    out.extend(struct.pack("<d", TEST_NUMBER))
    fn(f)
    return bytes(out)


def numbers(f):
    for t, v in f["k"]:
        if t == "n":
            yield v
    for p in f["p"]:
        yield from numbers(p)


def scan(dirs: list[Path]):
    tot = files = 0
    bad = {}
    big = {}
    for d in dirs:
        for p in sorted(d.glob("*.luac")):
            f = read_chunk(p.read_bytes())
            assert write_chunk64(f) == p.read_bytes(), f"reader not lossless on {p}"
            files += 1
            for v in numbers(f):
                tot += 1
                f32 = struct.unpack("<f", struct.pack("<f", v))[0]
                if f32 != v:
                    bad.setdefault(v, set()).add(f"{d.name}/{p.stem}")
                if abs(v) > 2 ** 24 and v == int(v):
                    big.setdefault(v, set()).add(f"{d.name}/{p.stem}")
    print(f"{files} chunks, {tot} number constants, all round-trip lossless through the reader")
    print(f"not exact in float32: {len(bad)} distinct values")
    for v, w in sorted(bad.items())[:40]:
        print(f"  {v!r:>24}  -> f32 {struct.unpack('<f', struct.pack('<f', v))[0]!r}  in {', '.join(sorted(w)[:4])}"
              f"{' ...' if len(w) > 4 else ''}")
    print(f"integers > 2^24: {len(big)}: {sorted(big)[:20]}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("paths", nargs="+")
    ap.add_argument("--number", choices=["float", "double"], default="double")
    ap.add_argument("--strip", action="store_true")
    ap.add_argument("--scan", action="store_true")
    a = ap.parse_args()
    if a.scan:
        scan([Path(p).expanduser() for p in a.paths])
        return
    src, dst = Path(a.paths[0]), Path(a.paths[1])
    b = src.read_bytes()
    f = read_chunk(b)
    assert write_chunk64(f) == b
    out = write_chunk(f, a.number, a.strip)
    dst.write_bytes(out)
    print(f"{src.name}: {len(b)} -> {len(out)} bytes ({a.number}{', stripped' if a.strip else ''})")


if __name__ == "__main__":
    sys.exit(main())
