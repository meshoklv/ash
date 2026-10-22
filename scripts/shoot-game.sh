#!/usr/bin/env bash
# Run the game EBOOT in autopilot (demo.txt) under PPSSPPHeadless, convert the raw frame dumps to PNG, a GIF and an mp4.
#   scripts/shoot-game.sh <cooked_dir> <out_dir> [dump_every=2] [frames=390] [emulated_seconds=40]
# cooked_dir must hold knight.ashc, knight.ashanim and hall_nochars.ashm + demon.ashc/.ashanim (M4) or
# hall_noknight.ashm (M3, demon baked in the scene); optional <scene>.pcol/.spawn (tools/cook_collision.py). Staged in build/ (outside git).
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"; repo="$(cd "$here/.." && pwd)"
cooked="$(realpath "$1")"; out="$2"; every="${3:-2}"; frames="${4:-390}"; secs="${5:-40}"
stage="$repo/build/stage-game"
rm -rf "$stage"; mkdir -p "$stage" "$out"
cp "$repo/build/src/game/EBOOT.PBP" "$stage/EBOOT.PBP"
# SCENE=arena (default: Asylum Demon c2232, upper hall) or SCENE=hall (the pit, Stray Demon c2230)
scene="${SCENE:-arena}"
demon=c2232; [ "$scene" = hall ] && demon=c2230
if [ -f "$cooked/$demon.ashc" ] && [ -f "$cooked/${scene}_nochars.ashm" ]; then
  cp "$cooked/${scene}_nochars.ashm" "$stage/scene.ashm"
  cp "$cooked/$demon.ashc" "$stage/demon.ashc"; cp "$cooked/$demon.ashanim" "$stage/demon.ashanim"
else
  cp "$cooked/hall_noknight.ashm" "$stage/scene.ashm"   # M3 layout
fi
cp "$cooked/knight.ashc" "$cooked/knight.ashanim" "$stage/"
[ -f "$cooked/$scene.pcol" ] && cp "$cooked/$scene.pcol" "$stage/scene.pcol"
[ -f "$cooked/$scene.spawn" ] && cp "$cooked/$scene.spawn" "$stage/scene.spawn"
echo "$every $frames" > "$stage/demo.txt"
# /dev/shm (64 MB) fits only one emulator: move this one's RAM arena to a file (scripts/ppsspp-shm-shim.c)
shim="$repo/build/ppsspp-shm-shim.so"
if [ ! -f "$shim" ]; then
  ( . "$here/env.sh"; x86_64-conda-linux-gnu-gcc -shared -fPIC -O2 -o "$shim" "$here/ppsspp-shm-shim.c" ) || true
fi
[ -f "$shim" ] && export LD_PRELOAD="$shim"
# one emulator at a time on the host (other agents run it too)
flock "$HOME/ash-assets/.emu.lock" "$here/run-headless.sh" "$stage/EBOOT.PBP" "$secs" "$out/run" || true
unset LD_PRELOAD
grep -E 'ash-game' "$out/run/run.log" | grep -v 'frame=' | head -60 || true
mkdir -p "$out/frames"
"$repo/tools/.venv/bin/python" -I - "$stage" "$out/frames" <<'PY'
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
    n += 1
print(f"frames converted: {n}")
PY
if command -v ffmpeg >/dev/null && ls "$out"/frames/fr_*.png >/dev/null 2>&1; then
  fps=$(( 30 / every ))
  nice -n 19 ffmpeg -loglevel error -y -framerate "$fps" -i "$out/frames/fr_%04d.png" \
    -vf "scale=360:-1:flags=lanczos,split[a][b];[a]palettegen=max_colors=96[p];[b][p]paletteuse=dither=bayer" "$out/game.gif"
  nice -n 19 ffmpeg -loglevel error -y -framerate "$fps" -i "$out/frames/fr_%04d.png" -pix_fmt yuv420p \
    -c:v libx264 -crf 22 "$out/game.mp4" 2>/dev/null || true
  ls -la "$out"/game.*
fi
