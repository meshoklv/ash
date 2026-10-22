#!/usr/bin/env bash
# PSP-est: run a scene in PPSSPPHeadless with the PSP-3000 cost model (src/perf, docs/psp-cost-model.md) and summarise.
#
#   scripts/psp-est.sh build                                          # psp-cmake src/perf -> build/perf (game/viewer/bench _est)
#   scripts/psp-est.sh viewer <scene.ashm> <out_dir> ["0 1 2 3"] [secs=4]
#   scripts/psp-est.sh game   <cooked_dir> <out_dir> [frames=390] [secs=40]   (autopilot demo.txt, SCENE=arena|hall)
#   scripts/psp-est.sh bench  <out_dir> [secs=400]                    # bench with log_draws=1 -> features for calibrate.py
#   scripts/psp-est.sh asylum <out_dir> <route|hollow|death|boss> [max_frames=0] [secs=1500] [dump_every=0]
#        "Ash: Asylum" (asy_main.c) autopilot with the model; ANALYZE (default 30) = analyse every Nth frame;
#        EBOOT=<pbp> runs another build (A/B), PAK=<zone pak> another zone package. Slot: scripts/emu-slot.sh.
#
# Env: PSP_COST_CFG=<file>  runtime coefficients (default src/perf/psp_cost.cfg); EXTRA_CFG="key = v;key = v" appended.
# One emulator at a time on the host: every run goes through flock ~/ash-assets/.emu.lock, RAM arena via the shm shim.
# Output: <out>/<run>/run.log (+ screen.png), <out>/<run>/pspest.json, summary on stdout.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"; repo="$(cd "$here/.." && pwd)"
py="$repo/tools/.venv/bin/python"; [ -x "$py" ] || py=python3
cfg="${PSP_COST_CFG:-$repo/src/perf/psp_cost.cfg}"
mode="${1:-}"; shift || true

build() {
  ( . "$here/env.sh"
    [ -f "$repo/build/perf/build.ninja" ] || psp-cmake -S "$repo/src/perf" -B "$repo/build/perf" -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null
    nice -n 19 cmake --build "$repo/build/perf" -j4 | grep -E 'error|FAILED' || true )
}

stage_cfg() {   # $1 = stage dir, $2.. extra "key = value"
  local st="$1"; shift
  cp "$cfg" "$st/psp_cost.cfg"
  for kv in "$@"; do echo "$kv" >> "$st/psp_cost.cfg"; done
  if [ -n "${EXTRA_CFG:-}" ]; then echo "$EXTRA_CFG" | tr ';' '\n' >> "$st/psp_cost.cfg"; fi
}

run() {   # $1 = eboot, $2 = secs, $3 = out
  local shim="$repo/build/ppsspp-shm-shim.so"
  if [ ! -f "$shim" ]; then ( . "$here/env.sh"; x86_64-conda-linux-gnu-gcc -shared -fPIC -O2 -o "$shim" "$here/ppsspp-shm-shim.c" ) || true; fi
  [ -f "$shim" ] && export LD_PRELOAD="$shim"
  nice -n 19 flock "$HOME/ash-assets/.emu.lock" "$here/run-headless.sh" "$1" "$2" "$3" >/dev/null || true
  unset LD_PRELOAD
}

case "$mode" in
build) build ;;
viewer)
  scene="$(realpath "$1")"; out="$2"; cams="${3:-0 1 2 3}"; secs="${4:-4}"
  build
  st="$repo/build/stage-est-viewer"; rm -rf "$st"; mkdir -p "$st" "$out"
  cp "$repo/build/perf/pbp_ash_viewer_est/EBOOT.PBP" "$st/"; cp "$scene" "$st/scene.ashm"; stage_cfg "$st"
  for c in $cams; do
    echo "$c" > "$st/camera.txt"
    run "$st/EBOOT.PBP" "$secs" "$out/cam$c"
    "$py" -I "$repo/tools/perf/report.py" "$out/cam$c/run.log" --label "viewer cam$c" --json "$out/cam$c/pspest.json" || true
  done ;;
game)
  cooked="$(realpath "$1")"; out="$2"; frames="${3:-390}"; secs="${4:-40}"
  build
  st="$repo/build/stage-est-game"; rm -rf "$st"; mkdir -p "$st" "$out"
  cp "$repo/build/perf/pbp_ash_game_est/EBOOT.PBP" "$st/"
  scene="${SCENE:-arena}"; demon=c2232; [ "$scene" = hall ] && demon=c2230
  if [ -f "$cooked/$demon.ashc" ] && [ -f "$cooked/${scene}_nochars.ashm" ]; then
    cp "$cooked/${scene}_nochars.ashm" "$st/scene.ashm"; cp "$cooked/$demon.ashc" "$st/demon.ashc"; cp "$cooked/$demon.ashanim" "$st/demon.ashanim"
  else cp "$cooked/hall_noknight.ashm" "$st/scene.ashm"; fi
  cp "$cooked/knight.ashc" "$cooked/knight.ashanim" "$st/"
  [ -f "$cooked/$scene.pcol" ] && cp "$cooked/$scene.pcol" "$st/scene.pcol"
  [ -f "$cooked/$scene.spawn" ] && cp "$cooked/$scene.spawn" "$st/scene.spawn"
  echo "${DUMP_EVERY:-0} $frames" > "$st/demo.txt"
  stage_cfg "$st"
  run "$st/EBOOT.PBP" "$secs" "$out/game-$scene"
  mkdir -p "$out/game-$scene/frames"; mv "$st"/fr_*.raw "$out/game-$scene/frames/" 2>/dev/null || true
  "$py" -I "$repo/tools/perf/report.py" "$out/game-$scene/run.log" --label "game $scene" --json "$out/game-$scene/pspest.json" ;;
bench)
  out="$1"; secs="${2:-400}"
  build
  st="$repo/build/stage-est-bench"; rm -rf "$st"; mkdir -p "$st" "$out"
  cp "$repo/build/perf/pbp_ash_bench_est/EBOOT.PBP" "$st/"
  stage_cfg "$st" "log_draws = 1" "overlay = 0" "log_every = 0" "mem_probe_every = 0"
  run "$st/EBOOT.PBP" "$secs" "$out/bench"
  "$py" -I "$repo/tools/perf/calibrate.py" features "$out/bench/run.log" --out "$out/bench_features.json" ;;
asylum)
  out="$1"; sc="$2"; frames="${3:-0}"; secs="${4:-1500}"; every="${5:-0}"
  [ -n "${EBOOT:-}" ] || build
  eb="${EBOOT:-$repo/build/perf/pbp_ash_asylum_est/EBOOT.PBP}"
  cooked="${ASH_COOKED:-$HOME/ash-assets/cooked}"
  st="$repo/build/stage-est-asylum-${TAG:-a}"; mkdir -p "$st" "$out/run"
  "$py" -I - "$st" <<'PY'
import sys, pathlib
d = pathlib.Path(sys.argv[1])
assert d.name.startswith("stage-est-asylum") and d.parent.name == "build"
for f in list(d.glob("fr_*.raw")) + [d / "ash.sav", d / "ash.sav.tmp"]:
    if f.exists(): f.unlink()
PY
  cp "$eb" "$st/EBOOT.PBP"
  cp "${PAK:-$cooked/world/zone_m18_01.pak}" "$st/zone_m18_01.pak"
  [ -f "$cooked/world/zone_m18_01.ashmip" ] && cp "$cooked/world/zone_m18_01.ashmip" "$st/"   # fast path (asy_fast.patch)
  for f in m18_01.ashevt combat.ashcb knight.ashc knight.ashanim knight_combat.ashanim knight_prg.ashanim hollow.ashc \
           hollow.ashanim boss.ashc boss.ashanim boss.ashboss boss_name.ashtx ui.ashui; do cp "${CHR_DIR:-$cooked}/$f" "$st/" 2>/dev/null || cp "$cooked/$f" "$st/"; done
  [ -d "$st/ai" ] || cp -r "$repo/build/stage-asylum/ai" "$st/ai"
  [ -f "$repo/build/stage-asylum/asy_bf2.sav" ] && cp "$repo/build/stage-asylum/asy_bf2.sav" "$st/"
  echo "$every $frames $sc ${OPT:-}" > "$st/demo.txt"
  stage_cfg "$st" "analyze_every = ${ANALYZE:-30}" "overlay = 0" "mem_probe_every = 0"
  shim="$repo/build/ppsspp-shm-shim.so"
  [ -f "$shim" ] && export LD_PRELOAD="$shim"
  ( . "$here/env.sh"; cd "$st"; "$here/emu-slot.sh" "$PPSSPP_HEADLESS" "$st/EBOOT.PBP" --graphics=software --root="$st" \
      --timeout-emulated="$secs" --timeout-wall=$(( secs * 8 + 60 )) --screenshot-save="$out/run/screen.png" \
      > "$out/run/run.log" 2>&1; echo "exit code: $?" >> "$out/run/run.log" ) || true
  unset LD_PRELOAD
  grep -aE '^ASY:' "$out/run/run.log" | tail -1 || true
  "$py" -I - "$st" "$out" <<'PY'
import sys, glob, os, pathlib
import numpy as np
from PIL import Image
src, dst = sys.argv[1], pathlib.Path(sys.argv[2]) / "frames"
dst.mkdir(exist_ok=True)
for f in sorted(glob.glob(os.path.join(src, "fr_*.raw"))):
    a = np.fromfile(f, "<u2")
    if a.size >= 512 * 272:
        a = a[:512 * 272].reshape(272, 512)[:, :480]
        r = ((a & 31) * 255 // 31).astype(np.uint8); g = (((a >> 5) & 63) * 255 // 63).astype(np.uint8)
        b = (((a >> 11) & 31) * 255 // 31).astype(np.uint8)
        Image.fromarray(np.dstack([r, g, b])).save(dst / (os.path.basename(f)[:-4] + ".png"))
    os.unlink(f)
PY
  grep -aE '^(PSPEST|ASY:|ash-asy)' "$out/run/run.log" > "$out/pspest.log" || true
  "$py" -I - "$out/run/run.log" <<'PY'
import sys, pathlib
p = pathlib.Path(sys.argv[1])
assert p.name == "run.log" and "ash-assets" in str(p)
p.unlink()
PY
  "$py" -I "$repo/tools/perf/report.py" "$out/pspest.log" --label "asylum $sc" --json "$out/pspest.json" || true ;;
*) sed -n 2,14p "$0"; exit 2 ;;
esac
