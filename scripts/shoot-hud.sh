#!/usr/bin/env bash
# Run the HUD / progression test EBOOT (src/hudtest) under PPSSPPHeadless, dump frames, convert to PNG + mp4.
#   scripts/shoot-hud.sh <cooked_dir> <out_dir> [dump_every=3] [frames=1420] [emulated_seconds=60] [keep_save=0]
# cooked_dir: scene (hall_nochars.ashm), knight.ashc/.ashanim, knight_prg.ashanim, ui.ashui, c2230.ashc/.ashanim.
# keep_save=1: do not delete ash.sav from the previous run (tests loading the save on start).
# One emulator at a time: run it under `flock ~/ash-assets/.emu.lock`. Emulated timings are not hardware timings.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"; repo="$(cd "$here/.." && pwd)"
cooked="$(realpath "$1")"; out="$2"; every="${3:-3}"; frames="${4:-1420}"; secs="${5:-60}"; keep="${6:-0}"
eboot="$repo/build/prg/src/hudtest/EBOOT.PBP"
stage="$repo/build/stage-hud"
mkdir -p "$stage" "$out"
rm -f "$stage"/fr_*.raw
[ "$keep" = 1 ] || rm -f "$stage/ash.sav"
cp "$eboot" "$stage/EBOOT.PBP"
cp "$cooked/hall_nochars.ashm" "$stage/scene.ashm"
for f in knight.ashc knight.ashanim knight_prg.ashanim ui.ashui demon.ashc demon.ashanim; do
  [ -f "$cooked/$f" ] && cp "$cooked/$f" "$stage/"
done
# the demon is cooked as c2230.* by tools/cook_demon.py now
[ -f "$stage/demon.ashc" ] || { [ -f "$cooked/c2230.ashc" ] && cp "$cooked/c2230.ashc" "$stage/demon.ashc" \
  && cp "$cooked/c2230.ashanim" "$stage/demon.ashanim"; } || true
echo "$every $frames" > "$stage/demo.txt"
shim="$repo/build/ppsspp-shm-shim.so"
if [ ! -f "$shim" ]; then
  ( . "$here/env.sh"; x86_64-conda-linux-gnu-gcc -shared -fPIC -O2 -o "$shim" "$here/ppsspp-shm-shim.c" ) || true
fi
[ -f "$shim" ] && export LD_PRELOAD="$shim"
"$here/run-headless.sh" "$stage/EBOOT.PBP" "$secs" "$out/run" || true
unset LD_PRELOAD
grep -E 'ash-hud' "$out/run/run.log" | head -80 || true
mkdir -p "$out/frames"
"$repo/tools/.venv/bin/python" -I - "$stage" "$out/frames" "$every" <<'PY'
import sys, glob, os
import numpy as np
from PIL import Image
src, dst, every = sys.argv[1], sys.argv[2], int(sys.argv[3])
n = 0
for f in sorted(glob.glob(os.path.join(src, "fr_*.raw"))):
    a = np.fromfile(f, "<u2")
    if a.size < 512 * 272:
        continue
    a = a[:512 * 272].reshape(272, 512)[:, :480]
    r = ((a & 31) * 255 // 31).astype(np.uint8)
    g = (((a >> 5) & 63) * 255 // 63).astype(np.uint8)
    b = (((a >> 11) & 31) * 255 // 31).astype(np.uint8)
    k = int(os.path.basename(f)[3:7]) * every   # simulation frame of this dump
    Image.fromarray(np.dstack([r, g, b])).save(os.path.join(dst, f"f{k:05d}.png"))
    n += 1
print(f"frames converted: {n}")
PY
rm -f "$stage"/fr_*.raw
if command -v ffmpeg >/dev/null && ls "$out"/frames/f*.png >/dev/null 2>&1; then
  nice -n 19 ffmpeg -loglevel error -y -framerate $(( 30 / every )) -pattern_type glob -i "$out/frames/f*.png" \
    -pix_fmt yuv420p -c:v libx264 -crf 22 "$out/hud.mp4" 2>/dev/null || true
  ls -la "$out"/hud.mp4 2>/dev/null || true
fi
