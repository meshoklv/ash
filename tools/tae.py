"""Minimal reader for DS1/DSR TAE files (version 0x1000B, 32-bit offsets, little-endian).

Soulstruct's TAE reader asserts version 0x1000C and 64-bit offsets, but the DSR `c0000*.anibnd` / `cXXXX.anibnd`
files on our copy are 0x1000B with 32-bit offsets [проверено: a23.tae header 0x0B 00 01 00, varints 0x40/1/0x50/0x70].
Layout (worked out on a23.tae, matches SoulsFormats' TAE3 with VarintLong = false):

  header: "TAE " 00 00 00 00 | i32 version | i32 file size | i32 0x40, 1, 0x50, 0x70 | ... |
          @0x50: i32 tae_id, i32 anim_count, i32 anims_offset
  anims_offset: anim_count x (i32 anim_id, i32 anim_offset)
  anim header (28 B): i32 event_count, events_offset, group_count, groups_offset, times_count, times_offset, file_offset
  event header (12 B): i32 start_time_offset, end_time_offset, data_offset  (times are f32 seconds)
  event data: i32 type, i32 params_offset, params...

Event argument layouts come from DSAnimStudio's TAE.Template.DS1.xml (Meowmaritus/DSAnimStudio, master).
"""
from __future__ import annotations

import struct


class TaeEvent:
    __slots__ = ("type", "start", "end", "params", "_d")

    def __init__(self, typ, start, end, params, d):
        self.type, self.start, self.end, self.params, self._d = typ, start, end, params, d

    def frames(self, fps: float = 30.0) -> tuple[int, int]:
        """[start, end) in 30 Hz frames. Vanilla events sit on 30 Hz ticks (combat-ai notes 4.3)."""
        return int(round(self.start * fps)), int(round(self.end * fps))

    def unpack(self, fmt: str, off: int = 0):
        return struct.unpack_from("<" + fmt, self._d, self.params + off)


def parse(data: bytes) -> tuple[int, int, dict[int, list[TaeEvent]]]:
    """-> (version, tae_id, {anim_id: [TaeEvent]})."""
    d = data
    if d[:4] != b"TAE " or d[4] != 0:
        raise ValueError("not a little-endian TAE")
    if d[7] != 0:
        raise ValueError("64-bit TAE: use soulstruct's reader")
    ver, = struct.unpack_from("<i", d, 8)
    tid, n, aoff = struct.unpack_from("<3i", d, 0x50)
    out: dict[int, list[TaeEvent]] = {}
    for i in range(n):
        aid, off = struct.unpack_from("<2i", d, aoff + 8 * i)
        ne, eh, ng, eg, nt, et, af = struct.unpack_from("<7i", d, off)
        evs = []
        for k in range(ne):
            so, eo, do = struct.unpack_from("<3i", d, eh + 12 * k)
            s, = struct.unpack_from("<f", d, so)
            e, = struct.unpack_from("<f", d, eo)
            typ, po = struct.unpack_from("<2i", d, do)
            evs.append(TaeEvent(typ, s, e, po, d))
        out[aid] = evs
    return ver, tid, out
