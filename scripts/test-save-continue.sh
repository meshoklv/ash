#!/usr/bin/env bash
# "Продолжить" across an emulator restart (src/save/test/continue): run 1 new game + bonfire autosave, run 2 boot
# finds the save -> main menu "Продолжить" -> status screen of the saved character. Shots -> <shots_dir>/*.png.
#   scripts/test-save-continue.sh [shots_dir]     needs build/savecont/EBOOT.PBP, ~/ash-assets/cooked/menu
set -uo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"; repo="$(cd "$here/.." && pwd)"
. "$here/env.sh"
shots="${1:-$HOME/ash-assets/shots/save-continue-$(date +%m%d-%H%M)}"; mkdir -p "$shots"
cooked="${ASH_COOKED:-$HOME/ash-assets/cooked}"
stage="$repo/build/stage-savecont"; ms="$repo/build/savecont-ms"
mkdir -p "$stage" "$ms"
cp "$repo/build/savecont/EBOOT.PBP" "$stage/EBOOT.PBP"
[ -f "$stage/menu.ashui" ] || cp "$cooked"/menu/menu.ashui "$cooked"/menu/menu.ashcls "$cooked"/menu/menu_idle.ashanim \
  "$cooked"/menu/cls_*.ashc "$cooked"/menu/head_*.ashc "$stage/"
cp "$cooked"/menu/menu.ashui "$cooked"/menu/menu.ashcls "$stage/"
py="$repo/tools/.venv/bin/python"
"$py" -I - "$ms" "$stage" <<'PY'
import sys, pathlib, shutil
ms, stage = pathlib.Path(sys.argv[1]).resolve(), pathlib.Path(sys.argv[2]).resolve()
for d in (ms / "PSP/SAVEDATA/ASHDS0001A", ms / "PSP/SAVEDATA/ASHDS0001B"):
    assert "/ash/build/" in str(d)
    if d.is_dir(): shutil.rmtree(d)
for p in list(stage.glob("sh_*.raw")) + [stage / "ash.sav", stage / "ash.sav.tmp"]:
    if p.exists(): p.unlink()
PY
shim="$repo/build/ppsspp-shm-shim.so"
[ -f "$shim" ] || x86_64-conda-linux-gnu-gcc -shared -fPIC -O2 -o "$shim" "$here/ppsspp-shm-shim.c"
for run in 1 2; do
  LD_PRELOAD="$shim" "$here/emu-slot.sh" "$PPSSPP_HEADLESS" "$stage/EBOOT.PBP" --graphics=software \
    --root="$stage" --memstick="$ms" --timeout-emulated=40 --timeout-wall=400 > "$stage/run-$run.log" 2>&1
  echo "== run $run"; grep 'ash-save' "$stage/run-$run.log" | grep -v ' shot '
done
"$py" -I - "$stage" "$shots" <<'PY'
import sys
from pathlib import Path
import numpy as np
from PIL import Image
stage, out = Path(sys.argv[1]).resolve(), Path(sys.argv[2])
assert "build" in stage.parts and stage.name == "stage-savecont", stage
for f in sorted(stage.glob("sh_*.raw")):
    a = np.fromfile(f, "<u2")
    if a.size >= 512 * 272:
        a = a[:512 * 272].reshape(272, 512)[:, :480]
        rgb = [((a & 31) * 255 // 31), (((a >> 5) & 63) * 255 // 63), (((a >> 11) & 31) * 255 // 31)]
        Image.fromarray(np.dstack(rgb).astype(np.uint8)).save(out / (f.stem[3:] + ".png"))
        print("shot", out / (f.stem[3:] + ".png"))
    f.unlink()
PY
