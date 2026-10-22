#!/usr/bin/env bash
# Run the items test EBOOT (src/items/test_psp) under PPSSPPHeadless and turn its named dumps into <out>/*.png.
#   src/items/test_psp/shoot.sh <out_dir> [emulated_seconds=40]
# Needs build/items-psp/EBOOT.PBP, ~/ash-assets/cooked/items (tools/cook_items.py), ~/ash-assets/cooked/menu
# (tools/cook_menu.py + cook_classes.py: menu.ashui, menu.ashcls, menu_idle.ashanim, cls_3001_m.ashc) and
# ~/ash-assets/cooked/hall_nochars.ashm. Runs through scripts/emu-slot.sh with the shm shim.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"; repo="$(cd "$here/../../.." && pwd)"
out="$1"; secs="${2:-40}"
cooked="${ASH_COOKED:-$HOME/ash-assets/cooked}"
stage="$repo/build/stage-items"
mkdir -p "$stage" "$out"
cp "$repo/build/items-psp/EBOOT.PBP" "$stage/EBOOT.PBP"
cp "$cooked"/items/items.ashit "$cooked"/items/items.ashui "$stage/"
cp "$cooked"/menu/menu.ashui "$cooked"/menu/menu.ashcls "$cooked"/menu/menu_idle.ashanim "$cooked"/menu/cls_3001_m.ashc "$stage/"
cp "$cooked/hall_nochars.ashm" "$stage/scene.ashm"
shim="$repo/build/ppsspp-shm-shim.so"
if [ ! -f "$shim" ]; then
  ( . "$repo/scripts/env.sh"; x86_64-conda-linux-gnu-gcc -shared -fPIC -O2 -o "$shim" "$repo/scripts/ppsspp-shm-shim.c" ) || true
fi
[ -f "$shim" ] && export LD_PRELOAD="$shim"
"$repo/scripts/emu-slot.sh" "$repo/scripts/run-headless.sh" "$stage/EBOOT.PBP" "$secs" "$out/run" || true
unset LD_PRELOAD
"$repo/tools/.venv/bin/python" -I - "$stage" "$out" <<'PY'
import sys
from pathlib import Path
import numpy as np
from PIL import Image
stage, out = Path(sys.argv[1]).resolve(), Path(sys.argv[2])
assert "build" in stage.parts and stage.name == "stage-items", stage
n = 0
for f in sorted(stage.glob("sh_*.raw")):
    a = np.fromfile(f, "<u2")
    if a.size >= 512 * 272:
        a = a[:512 * 272].reshape(272, 512)[:, :480]
        r = ((a & 31) * 255 // 31).astype(np.uint8)
        g = (((a >> 5) & 63) * 255 // 63).astype(np.uint8)
        b = (((a >> 11) & 31) * 255 // 31).astype(np.uint8)
        Image.fromarray(np.dstack([r, g, b])).save(out / (f.stem[3:] + ".png"))
        n += 1
    f.unlink()
print(f"shots {n}")
# the emulator log is huge (-l): keep only our lines
log = out / "run" / "run.log"
if log.exists():
    keep = [l for l in log.read_text(errors="replace").splitlines()
            if "ash-items" in l or "Frame stats" in l or "exit code" in l]
    (out / "items.log").write_text("\n".join(keep) + "\n")
    log.unlink()
shot = out / "run" / "screen.png"
if shot.exists():
    shot.unlink()
PY
grep -E 'ash-items' "$out/items.log" | head -80 || true
