#!/usr/bin/env bash
# Boss test EBOOT (src/boss/bosstest.c) under PPSSPPHeadless: frame dumps -> PNG, GIF, mp4.
#   scripts/shoot-boss.sh <cooked_dir> <out_dir> [dump_every=10] [frames=1800] [emulated_seconds=70] [lua]
# "lua": stage the original DSR AI bytecode (aiCommon + 223200_battle, repacked 32-bit float, ~/ash-assets/ai) and
#        drive the demon with it (src/boss/boss_ai.c on the src/ai host) instead of the C state machine.
# cooked_dir: arena_nochars.ashm (cook_scene --scene arena --no-chars), knight.ashc/.ashanim,
#             boss.ashc/.ashanim/.ashboss + boss_name.ashtx (tools/cook_boss.py). Staged in build/ (outside git).
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"; repo="$(cd "$here/.." && pwd)"
cooked="$(realpath "$1")"; out="$2"; every="${3:-10}"; frames="${4:-1800}"; secs="${5:-70}"; mode="${6:-fsm}"
stage="$repo/build/stage-boss"
rm -rf "$stage"; mkdir -p "$stage" "$out"
cp "$repo/build/boss-psp/EBOOT.PBP" "$stage/EBOOT.PBP"
cp "$cooked/arena_nochars.ashm" "$stage/scene.ashm"
cp "$cooked/knight.ashc" "$cooked/knight.ashanim" "$cooked/boss.ashc" "$cooked/boss.ashanim" "$cooked/boss.ashboss" \
   "$cooked/boss_name.ashtx" "$stage/"
[ -f "$cooked/arena.pcol" ] && cp "$cooked/arena.pcol" "$stage/"
echo "$every $frames" > "$stage/demo.txt"
if [ "$mode" = lua ]; then
  ai="$HOME/ash-assets/ai"; py="$repo/tools/.venv/bin/python"
  mkdir -p "$stage/ai/aiCommon" "$stage/ai/m18_01_00_00"
  for f in "$ai"/aiCommon/*.luac; do
    "$py" "$repo/tools/ai/luac_repack.py" "$f" "$stage/ai/aiCommon/$(basename "$f")" --number float --strip >/dev/null
  done
  "$py" "$repo/tools/ai/luac_repack.py" "$ai/m18_01_00_00/223200_battle.luac" \
    "$stage/ai/m18_01_00_00/223200_battle.luac" --number float --strip >/dev/null
  cp "$ai/aiCommon/load_order.txt" "$ai/think_223200.txt" "$stage/ai/" 2>/dev/null || true
  mv "$stage/ai/load_order.txt" "$stage/ai/aiCommon/"
  echo lua > "$stage/ai/lua.txt"
fi
shim="$repo/build/ppsspp-shm-shim.so"
if [ ! -f "$shim" ]; then
  ( . "$here/env.sh"; x86_64-conda-linux-gnu-gcc -shared -fPIC -O2 -o "$shim" "$here/ppsspp-shm-shim.c" ) || true
fi
[ -f "$shim" ] && export LD_PRELOAD="$shim"
flock "$HOME/ash-assets/.emu.lock" "$here/run-headless.sh" "$stage/EBOOT.PBP" "$secs" "$out/run" || true
unset LD_PRELOAD
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
  fps=$(( 30 / every > 0 ? 30 / every : 1 ))
  nice -n 19 ffmpeg -loglevel error -y -framerate "$fps" -i "$out/frames/fr_%04d.png" \
    -vf "scale=360:-1:flags=lanczos,split[a][b];[a]palettegen=max_colors=96[p];[b][p]paletteuse=dither=bayer" "$out/boss.gif" || true
  nice -n 19 ffmpeg -loglevel error -y -framerate "$fps" -i "$out/frames/fr_%04d.png" -pix_fmt yuv420p \
    -c:v libx264 -crf 22 "$out/boss.mp4" 2>/dev/null || true
fi
