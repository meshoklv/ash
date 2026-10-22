#!/usr/bin/env bash
# Build ash_aitest for the PSP, stage repacked (32-bit, float, stripped) DSR AI bytecode next to it, run it in
# PPSSPPHeadless and print the log. Same scenario as the PC prototype (build/host/ai_proto), so logs compare.
#   tools/ai/stage_psp.sh [ai_dir=~/ash-assets/ai] [goal=250000] [emulated_seconds=60]
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"; repo="$(cd "$here/../.." && pwd)"
ai="$(realpath "${1:-$HOME/ash-assets/ai}")"; goal="${2:-250000}"; secs="${3:-60}"
. "$repo/scripts/env.sh"
psp-cmake -S "$repo/src/ai" -B "$repo/build/ai-psp" -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null
nice -n 19 cmake --build "$repo/build/ai-psp" -j4 >/dev/null
stage="$repo/build/stage-ai"
rm -rf "$stage"; mkdir -p "$stage/ai/aiCommon" "$stage/ai/m18_01_00_00"
cp "$repo/build/ai-psp/EBOOT.PBP" "$stage/"
py="$repo/tools/.venv/bin/python"
for f in "$ai"/aiCommon/*.luac; do
  "$py" "$here/luac_repack.py" "$f" "$stage/ai/aiCommon/$(basename "$f")" --number float --strip >/dev/null
done
"$py" "$here/luac_repack.py" "$ai/m18_01_00_00/$(printf %06d "$goal")_battle.luac" \
  "$stage/ai/m18_01_00_00/$(printf %06d "$goal")_battle.luac" --number float --strip
cp "$ai"/think_*.txt "$stage/ai/"
cp "$ai/aiCommon/load_order.txt" "$stage/ai/aiCommon/"
du -sh "$stage/ai"
shim="$repo/build/ppsspp-shm-shim.so"
[ -f "$shim" ] || x86_64-conda-linux-gnu-gcc -shared -fPIC -O2 -o "$shim" "$repo/scripts/ppsspp-shm-shim.c"
out="$repo/build/stage-ai/run.log"
set +e
LD_PRELOAD="$shim" flock "$HOME/ash-assets/.emu.lock" nice -n 10 "$PPSSPP_HEADLESS" "$stage/EBOOT.PBP" \
  --graphics=software --root="$stage" --timeout-emulated="$secs" --timeout-wall=$((secs * 6 + 60)) -l > "$out" 2>&1
echo "emulator exit $?" >> "$out"
set -e
grep -vE '^\s*$|^[0-9]+:[0-9]+:' "$out" | grep -vE 'HLE|sceKernel|Loader|^\[' | tail -n +1
