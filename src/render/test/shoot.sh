#!/usr/bin/env bash
# Render test "before/after" (docs/render.md §9): stage one scene + cooked sidecars, run ash_rtest(_est) in
# PPSSPPHeadless under the shared emulator lock, convert dumps to PNG, side-by-side stills and a before|after mp4.
#   src/render/test/shoot.sh <scene: arena|hall> <out_dir> [orbit_frames=90] [warmup=45]
# Needs in ~/ash-assets/cooked: <scene>_nochars.ashm, <scene>_nochars.ashmip (tools/texmip.py),
# <scene>.ashl (tools/cook_light.py), <scene>.pcol, <scene>.spawn, knight.*, c2232.* (arena) / c2230.* (hall).
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"; repo="$(cd "$here/../../.." && pwd)"
scene="$1"; out="$2"; orbit="${3:-90}"; warm="${4:-45}"
ck="${ASH_COOKED:-$HOME/ash-assets/cooked}"
demon=c2232; [ "$scene" = hall ] && demon=c2230
eboot="$repo/build/render/pbp_rtest_est/EBOOT.PBP"; [ -f "$eboot" ] || eboot="$repo/build/render/pbp_rtest/EBOOT.PBP"
stage="$repo/build/render/stage-$scene"
mkdir -p "$stage" "$out"
"$repo/tools/.venv/bin/python" -I "$here/post.py" --clean "$stage"   # old dumps out (python, path-checked)
cp "$eboot" "$stage/EBOOT.PBP"
cp "$ck/${scene}_nochars.ashm" "$stage/scene.ashm"
[ -f "$ck/${scene}_nochars.ashmip" ] && cp "$ck/${scene}_nochars.ashmip" "$stage/scene.ashmip"
[ -f "$ck/$scene.ashl" ] && cp "$ck/$scene.ashl" "$stage/scene.ashl"
[ -f "$ck/${scene}_vlit.ashm" ] && cp "$ck/${scene}_vlit.ashm" "$stage/scene_vlit.ashm"
cp "$ck/$scene.pcol" "$stage/scene.pcol"; cp "$ck/$scene.spawn" "$stage/scene.spawn"
cp "$ck/knight.ashc" "$ck/knight.ashanim" "$stage/"
cp "$ck/$demon.ashc" "$stage/demon.ashc"; cp "$ck/$demon.ashanim" "$stage/demon.ashanim"
echo "$orbit $warm" > "$stage/rtest.txt"
echo "overlay = 0" > "$stage/psp_cost.cfg"   # src/perf: no PSP-est text in the shots
shim="$repo/build/ppsspp-shm-shim.so"
if [ ! -f "$shim" ]; then
  ( . "$repo/scripts/env.sh"; x86_64-conda-linux-gnu-gcc -shared -fPIC -O2 -o "$shim" "$repo/scripts/ppsspp-shm-shim.c" ) || true
fi
[ -f "$shim" ] && export LD_PRELOAD="$shim"
secs=$(( (orbit * 10 / 3 + 20 * 2 + (warm + 10) * 4) / 2 + 20 ))   # PSP-est build: ~0.45 emulated s per frame
flock "$HOME/ash-assets/.emu.lock" "$repo/scripts/run-headless.sh" "$stage/EBOOT.PBP" "$secs" "$out/run" || true
unset LD_PRELOAD
grep -E '^rtest|rtest:' "$out/run/run.log" | head -40 || true
"$repo/tools/.venv/bin/python" -I "$here/post.py" "$stage" "$out" "$scene"
[ -n "${KEEP_RAW:-}" ] || "$repo/tools/.venv/bin/python" -I "$here/post.py" --clean "$stage"
