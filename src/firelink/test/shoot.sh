#!/usr/bin/env bash
# Firelink test run (docs/firelink.md): stage the zone + sidecars + actors, run the test EBOOT in PPSSPPHeadless under
# the shared emulator lock (+ shm shim), convert dumps to PNG / mp4.
#   src/firelink/test/shoot.sh <stills|video> <out_dir> [secs]
#   stills -> PSP-est build (PSPEST per still), video -> plain build (PSP-est makes every frame ~10x slower)
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"; repo="$(cd "$here/../../.." && pwd)"
mode="$1"; out="$2"
ck="${ASH_COOKED:-$HOME/ash-assets/cooked}"; fz="${FL_ZONE:-$ck/firelink}"
py="$repo/tools/.venv/bin/python"
if [ "$mode" = stills ]; then eboot="$repo/build/firelink/pbp_fltest_est/EBOOT.PBP"; secs="${3:-60}"
else eboot="$repo/build/firelink/pbp_fltest/EBOOT.PBP"; secs="${3:-120}"; fi
stage="$repo/build/firelink/stage-$mode"
mkdir -p "$stage" "$out"
"$py" -I "$here/post.py" --clean "$stage"
cp "$eboot" "$stage/EBOOT.PBP"
cp "$fz/zone_m10_02.pak" "$stage/zone.pak"; cp "$fz/zone_m10_02.ashmip" "$stage/zone.ashmip"
cp "$fz/zone_m10_02.cfg" "$stage/zone.cfg"
[ -f "$here/look.txt" ] && cat "$here/look.txt" >> "$stage/zone.cfg"
cp "$ck/knight.ashc" "$ck/knight.ashanim" "$stage/"
cp "$ck/firelink/crestfallen.ashc" "$ck/firelink/crestfallen.ashanim" "$stage/"
cp "$here/$mode.txt" "$stage/fl.txt"
echo "overlay = 0" > "$stage/psp_cost.cfg"
shim="$repo/build/ppsspp-shm-shim.so"
if [ ! -f "$shim" ]; then
  ( . "$repo/scripts/env.sh"; x86_64-conda-linux-gnu-gcc -shared -fPIC -O2 -o "$shim" "$repo/scripts/ppsspp-shm-shim.c" ) || true
fi
[ -f "$shim" ] && export LD_PRELOAD="$shim"
flock "$HOME/ash-assets/.emu.lock" "$repo/scripts/run-headless.sh" "$stage/EBOOT.PBP" "$secs" "$out/run" || true
unset LD_PRELOAD
grep -E 'fl:' "$out/run/run.log" | head -12 || true
"$py" -I "$here/post.py" "$stage" "$out"
[ -n "${KEEP_RAW:-}" ] || "$py" -I "$here/post.py" --clean "$stage"
