"""Minimal reader for DS1/DSR TAE files (version 0x1000B, 32-bit offsets) — soulstruct only reads 0x1000C.

Layout [проверено на c2230.tae / c2232.tae из DSR]:
  header: 'TAE ', ..., +0x50 int32 tae_id, +0x54 int32 anim_count, +0x58 int32 anim_table_offset
  anim table: anim_count x (int32 anim_id, int32 anim_offset)
  anim:  int32 event_count, event_headers_off, group_count, groups_off, times_count, times_off, anim_file_off
  event header: int32 start_time_off, end_time_off, event_data_off -> times are float32 seconds
  event data: int32 type, int32 params_off -> params (type-specific, read here as raw int32/float32)
Event meanings used by Ash (DSAnimStudio DS1 template, [версия] where marked in docs/boss.md):
  type 0  JumpTable: params[0] = flag id (7 turning?, 23/78/79/86 cancel windows, 8/24/27/41 i-frames/super armor…)
  type 1  InvokeAttackBehavior: params[2] = BehaviorJudgeID  -> BehaviorParam 200000000 + variation*1000 + judge
  type 2  InvokeBulletBehavior: params[2] = BehaviorJudgeID (bullet / blast)
  type 5  InvokeCommonBehavior: params[1] = BehaviorParam id (common, 20xxx)
"""
from __future__ import annotations

import struct
from dataclasses import dataclass, field


@dataclass
class TaeEvent:
    type: int
    start: float
    end: float
    ints: tuple
    floats: tuple


@dataclass
class TaeAnim:
    id: int
    events: list = field(default_factory=list)
    is_ref: bool = False


def read_tae(data: bytes) -> dict[int, TaeAnim]:
    assert data[:4] == b"TAE ", "not a TAE"
    ver = struct.unpack_from("<i", data, 8)[0]
    assert ver == 0x1000B, f"TAE version {ver:#x}: only DS1 0x1000B supported"
    d = data + b"\0" * 32  # params near the end of the file are read with a fixed window
    u = lambda o, f="<i": struct.unpack_from(f, d, o)[0]  # noqa: E731
    n, ao = u(0x54), u(0x58)
    out = {}
    for i in range(n):
        aid, off = u(ao + 8 * i), u(ao + 8 * i + 4)
        ec, eh, _gc, _go, _tc, _to, af = struct.unpack_from("<7i", d, off)
        a = TaeAnim(aid, is_ref=bool(af and d[af]))
        for k in range(ec):
            so, eo, do = struct.unpack_from("<3i", d, eh + 12 * k)
            ty, po = u(do), u(do + 4)
            ints = struct.unpack_from("<4i", d, po) if po else ()
            fl = struct.unpack_from("<4f", d, po) if po else ()
            a.events.append(TaeEvent(ty, u(so, "<f"), u(eo, "<f"), ints, fl))
        out[aid] = a
    return out
