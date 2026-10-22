#!/usr/bin/env bash
# Stage a cooked scene next to the viewer EBOOT (outside git: build/ + ~/ash-assets) and screenshot each camera.
#   scripts/shoot-viewer.sh <scene.ashm> <out_dir> [cams="0 1 2 3"] [seconds=4]
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"; repo="$(cd "$here/.." && pwd)"
scene="$(realpath "$1")"; out="$2"; cams="${3:-0 1 2 3}"; secs="${4:-4}"
stage="$repo/build/stage-viewer"
mkdir -p "$stage" "$out"
cp "$repo/build/src/viewer/EBOOT.PBP" "$stage/EBOOT.PBP"
cp "$scene" "$stage/scene.ashm"
for c in $cams; do
  echo "$c" > "$stage/camera.txt"
  "$here/run-headless.sh" "$stage/EBOOT.PBP" "$secs" "$out/cam$c" || true
done
