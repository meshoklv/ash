#!/usr/bin/env bash
# "Ash: Asylum" EBOOT (src/game/asy_main.c) under PPSSPPHeadless with the autopilot: frame dumps -> PNG, mp4.
#   scripts/shoot-asylum.sh <out_dir> <route|hollow|death|boss> [dump_every=6] [max_frames=0] [emulated_seconds=900]
# Stages the cooked data (~/ash-assets/cooked, outside git) + the DSR AI bytecode repacked to 32-bit float
# (~/ash-assets/ai) in build/stage-asylum. One emulator at a time on the host: flock ~/ash-assets/.emu.lock.
# Scenario "boss" starts from the snapshot asy_bf2.sav that "route" writes at bonfire 2 (kept in the stage dir).
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"; repo="$(cd "$here/.." && pwd)"
out="$1"; sc="$2"; every="${3:-6}"; frames="${4:-0}"; secs="${5:-900}"
cooked="${ASH_COOKED:-$HOME/ash-assets/cooked}"; ai="$HOME/ash-assets/ai"; py="$repo/tools/.venv/bin/python"
stage="$repo/build/stage-asylum"
mkdir -p "$stage/ai/aiCommon" "$stage/ai/m18_01_00_00" "$out"
"$py" -I - "$stage" <<'PY'
import sys, pathlib
d = pathlib.Path(sys.argv[1])
assert d.name == "stage-asylum" and d.parent.name == "build"
for f in d.glob("fr_*.raw"): f.unlink()
for f in [d / "ash.sav", d / "ash.sav.tmp"]:
    if f.exists(): f.unlink()
PY
cp "$repo/build/src/game/asylum/EBOOT.PBP" "$stage/EBOOT.PBP"
for f in world/zone_m18_01.pak m18_01.ashevt combat.ashcb knight.ashc knight.ashanim knight_combat.ashanim \
         knight_prg.ashanim hollow.ashc hollow.ashanim boss.ashc boss.ashanim boss.ashboss boss_name.ashtx ui.ashui; do
  cp "$cooked/$f" "$stage/"
done
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
[ -f "$shim" ] && export LD_PRELOAD="$shim"
# no emulator debug log (-l): a 15-minute run writes ~4 GB of it; the game's stdout comes through anyway
mkdir -p "$out/run"
( . "$here/env.sh"; flock "$HOME/ash-assets/.emu.lock" nice -n 10 "$PPSSPP_HEADLESS" "$stage/EBOOT.PBP" --graphics=software \
    --root="$stage" --timeout-emulated="$secs" --timeout-wall=$(( secs * 8 + 60 )) \
    --screenshot-save="$out/run/screen.png" > "$out/run/run.log" 2>&1; echo "exit code: $?" >> "$out/run/run.log" ) || true
unset LD_PRELOAD
grep -aE '^ASY:' "$out/run/run.log" | tail -1 || true
mkdir -p "$out/frames"
"$py" -I - "$stage" "$out/frames" <<'PY'
import sys, glob, os
import numpy as np
from PIL import Image
src, dst = sys.argv[1], sys.argv[2]
n = 0
for f in sorted(glob.glob(os.path.join(src, "fr_*.raw"))):
    a = np.fromfile(f, "<u2")
    if a.size < 512 * 272:
        continue
    a = a[:512 * 272].reshape(272, 512)[:, :480]
    r = ((a & 31) * 255 // 31).astype(np.uint8)
    g = (((a >> 5) & 63) * 255 // 63).astype(np.uint8)
    b = (((a >> 11) & 31) * 255 // 31).astype(np.uint8)
    Image.fromarray(np.dstack([r, g, b])).save(os.path.join(dst, os.path.basename(f)[:-4] + ".png"))
    os.unlink(f)
    n += 1
print(f"frames converted: {n}")
PY
if command -v ffmpeg >/dev/null && ls "$out"/frames/fr_*.png >/dev/null 2>&1; then
  fps=$(( 30 / every > 0 ? 30 / every : 1 ))
  nice -n 19 ffmpeg -loglevel error -y -framerate "$fps" -i "$out/frames/fr_%04d.png" -pix_fmt yuv420p \
    -c:v libx264 -crf 23 "$out/asylum-$sc.mp4" 2>/dev/null || true
  ls -la "$out/asylum-$sc.mp4" 2>/dev/null || true
fi
