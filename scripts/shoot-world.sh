#!/usr/bin/env bash
# Camera tour of a cooked zone in PPSSPPHeadless: stage the world test EBOOT + pak + cams.txt, run once
# (one emulator at a time: flock ~/ash-assets/.emu.lock), convert the dumped frames to PNG + a contact sheet.
#   [ASH_VIEW="far lod fog_near fog_far"] [ASH_CAMS=cams.txt] [ASH_STAGE=dir] scripts/shoot-world.sh <zone.pak> <out_dir> [--only 0,3,5]
# EBOOT: build/world/src/world/EBOOT.PBP (psp-cmake -B build/world && cmake --build build/world --target ash_world_test)
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"; repo="$(cd "$here/.." && pwd)"
pak="$(realpath "$1")"; out="$2"; shift 2
stage="${ASH_STAGE:-$repo/build/stage-world}"   # own ASH_STAGE when several agents shoot at once
rm -rf "$stage"; mkdir -p "$stage" "$out"
cp "$repo/build/world/src/world/EBOOT.PBP" "$stage/EBOOT.PBP"
cp "$pak" "$stage/zone_m18_01.pak"
if [ -n "${ASH_CAMS:-}" ]; then cp "$ASH_CAMS" "$stage/cams.txt"   # own cameras (same format), e.g. game-like views
else "$repo/tools/.venv/bin/python" "$repo/tools/world_cams.py" "$pak" "$stage/cams.txt" "$@" > /dev/null; fi
echo 1 > "$stage/auto.txt"
[ -n "${ASH_VIEW:-}" ] && echo "$ASH_VIEW" > "$stage/view.txt"   # "<far> <lod> <fog_near> <fog_far>"
ncam=$(grep -vc '^#' "$stage/cams.txt")
shim="$repo/build/ppsspp-shm-shim.so"
if [ ! -f "$shim" ]; then
  ( . "$here/env.sh"; x86_64-conda-linux-gnu-gcc -shared -fPIC -O2 -o "$shim" "$here/ppsspp-shm-shim.c" ) || true
fi
[ -f "$shim" ] && export LD_PRELOAD="$shim"
secs=$(( ncam / 10 + 8 ))
flock "$HOME/ash-assets/.emu.lock" "$here/run-headless.sh" "$stage/EBOOT.PBP" "$secs" "$out/run" || true
unset LD_PRELOAD
grep -E 'ash-world' "$out/run/run.log" | head -80 || true
mkdir -p "$out/frames"
"$repo/tools/.venv/bin/python" -I - "$stage" "$out" <<'PY'
import sys, glob, os
import numpy as np
from PIL import Image, ImageDraw
src, dst = sys.argv[1], sys.argv[2]
labels = [l.split()[0] for l in open(os.path.join(src, "cams.txt")) if l.strip() and not l.startswith("#")]
ims = []
for i, f in enumerate(sorted(glob.glob(os.path.join(src, "fr_*.raw")))):
    a = np.fromfile(f, "<u2")
    if a.size < 512 * 272:
        continue
    a = a[:512 * 272].reshape(272, 512)[:, :480]
    r = ((a & 31) * 255 // 31).astype(np.uint8)
    g = (((a >> 5) & 63) * 255 // 63).astype(np.uint8)
    b = (((a >> 11) & 31) * 255 // 31).astype(np.uint8)
    im = Image.fromarray(np.dstack([r, g, b]))
    name = f"{i:02d}_{labels[i] if i < len(labels) else 'cam'}.png"
    im.save(os.path.join(dst, "frames", name))
    ims.append((name, im))
if ims:
    cols = 3
    rows = (len(ims) + cols - 1) // cols
    sheet = Image.new("RGB", (cols * 480, rows * 272))
    for k, (name, im) in enumerate(ims):
        sheet.paste(im, ((k % cols) * 480, (k // cols) * 272))
    sheet.save(os.path.join(dst, "sheet.png"))
print(f"frames converted: {len(ims)}")
PY
