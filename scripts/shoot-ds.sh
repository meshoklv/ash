#!/usr/bin/env bash
# "Ash: Dark Souls" EBOOT (src/game/ds/ds_main.c) under PPSSPPHeadless with the autopilot -> named frames as PNG.
#   scripts/shoot-ds.sh <out_dir> <full|hollow|boss|death|continue|firelink|flrun|probe|vhollow> [dump_every=0] [max_frames=0]
#                       [emulated_seconds=1500] [keep_ms=0]
# Stages the cooked data (~/ash-assets/cooked, outside git; big files as symlinks) + the DSR AI bytecode repacked to
# 32-bit float in build/stage-ds-$DS_TAG (default a). The memory stick is build/ds-ms-$DS_TAG (savedata A/B live there): keep_ms=1 keeps it
# (run "continue" after "full"), 0 wipes it first. Emulator slot: scripts/emu-slot.sh (<=3 at once, RAM check).
# DS_EBOOT=<path> runs another build (default build/ds); its ../../diag/ashexc.prx is staged too.
# Output: <out_dir>/<name>.png for every sh_<name>.raw (+ fr_NNNN.png when dump_every>0), run.log, the DS:{...} line.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"; repo="$(cd "$here/.." && pwd)"
out="$1"; sc="$2"; every="${3:-0}"; frames="${4:-0}"; secs="${5:-1500}"; keep="${6:-0}"
cooked="${ASH_COOKED:-$HOME/ash-assets/cooked}"; ai="$HOME/ash-assets/ai"; py="$repo/tools/.venv/bin/python"
tag="${DS_TAG:-a}"; stage="$repo/build/stage-ds-$tag"; ms="$repo/build/ds-ms-$tag"
mkdir -p "$stage/ai/aiCommon" "$stage/ai/m18_01_00_00" "$ms" "$out"
"$py" -I - "$stage" "$ms" "$keep" <<'PY'
import sys, pathlib, shutil
d, ms, keep = pathlib.Path(sys.argv[1]).resolve(), pathlib.Path(sys.argv[2]).resolve(), sys.argv[3] == "1"
assert d.name.startswith("stage-ds-") and d.parent.name == "build" and ms.name.startswith("ds-ms-") and ms.parent.name == "build"
for pat in ("fr_*.raw", "sh_*.raw"):
    for f in d.glob(pat): f.unlink()
for f in (d / "ash.sav", d / "ash.sav.tmp"):
    if f.exists() and not keep: f.unlink()
if not keep:
    for s in ("ASHDS0001A", "ASHDS0001B"):
        p = ms / "PSP/SAVEDATA" / s
        if p.is_dir(): shutil.rmtree(p)
PY
cp "${DS_EBOOT:-$repo/build/ds/src/game/ds/EBOOT.PBP}" "$stage/EBOOT.PBP"   # DS_EBOOT: another build
prx="$(dirname "${DS_EBOOT:-$repo/build/ds/src/game/ds/EBOOT.PBP}")/../../diag/ashexc.prx"   # src/diag kernel helper
[ -f "$prx" ] && cp "$prx" "$stage/ashexc.prx"
link() { ln -sf "$1" "$stage/$(basename "$1")"; }
for f in world/zone_m18_01.pak world/zone_m18_01.ashmip m18_01.ashevt combat.ashcb knight.ashc knight.ashanim \
         knight_combat.ashanim knight_prg.ashanim hollow.ashc hollow.ashanim hollow_combat.ashanim boss.ashc \
         boss.ashanim boss.ashboss boss_name.ashtx ui.ashui items/items.ashit items/items.ashui \
         firelink/zone_m10_02.pak firelink/zone_m10_02.ashmip firelink/crestfallen.ashc firelink/crestfallen.ashanim \
         firelink/firelink.txt menu/menu.ashui menu/menu.ashcls menu/menu_idle.ashanim; do
  [ -f "$cooked/$f" ] && link "$cooked/$f" || echo "WARN: no $cooked/$f"
done
for f in "$cooked"/menu/cls_*.ashc "$cooked"/menu/head_*.ashc; do link "$f"; done
if [ ! -f "$stage/ai/aiCommon/load_order.txt" ] || [ "$ai/aiCommon/load_order.txt" -nt "$stage/ai/aiCommon/load_order.txt" ]; then
  for f in "$ai"/aiCommon/*.luac; do
    "$py" "$repo/tools/ai/luac_repack.py" "$f" "$stage/ai/aiCommon/$(basename "$f")" --number float --strip >/dev/null
  done
  for b in 223200 250000; do
    "$py" "$repo/tools/ai/luac_repack.py" "$ai/m18_01_00_00/${b}_battle.luac" "$stage/ai/m18_01_00_00/${b}_battle.luac" \
      --number float --strip >/dev/null
  done
  cp "$ai/aiCommon/load_order.txt" "$stage/ai/aiCommon/"
  cp "$ai/think_223200.txt" "$ai/think_250022.txt" "$stage/ai/"
fi
echo "$every $frames $sc ${OPT:-}" > "$stage/demo.txt"
shim="$repo/build/ppsspp-shm-shim.so"
if [ ! -f "$shim" ]; then
  ( . "$here/env.sh"; x86_64-conda-linux-gnu-gcc -shared -fPIC -O2 -o "$shim" "$here/ppsspp-shm-shim.c" ) || true
fi
. "$here/env.sh"
set +e
LD_PRELOAD="$shim" "$here/emu-slot.sh" "$PPSSPP_HEADLESS" "$stage/EBOOT.PBP" --graphics=software --root="$stage" \
  --memstick="$ms" --timeout-emulated="$secs" --timeout-wall=$(( secs * 8 + 120 )) > "$out/run.log" 2>&1
echo "exit code: $?" >> "$out/run.log"
set -e
grep -aE '^DS:' "$out/run.log" | tail -1 || echo "no DS: line"
"$py" -I - "$stage" "$out" <<'PY'
import sys
from pathlib import Path
import numpy as np
from PIL import Image
stage, out = Path(sys.argv[1]).resolve(), Path(sys.argv[2])
assert stage.name.startswith("stage-ds-") and stage.parent.name == "build"
n = 0
for f in sorted(list(stage.glob("sh_*.raw")) + list(stage.glob("fr_*.raw"))):
    a = np.fromfile(f, "<u2")
    if a.size >= 512 * 272:
        a = a[:512 * 272].reshape(272, 512)[:, :480]
        rgb = [((a & 31) * 255 // 31), (((a >> 5) & 63) * 255 // 63), (((a >> 11) & 31) * 255 // 31)]
        name = f.stem[3:] if f.name.startswith("sh_") else f.stem
        Image.fromarray(np.dstack(rgb).astype(np.uint8)).save(out / (name + ".png"))
        n += 1
    f.unlink()
print(f"frames: {n} PNG in {out}")
PY
