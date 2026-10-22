#!/usr/bin/env bash
# Run the combat test EBOOT (src/combat/test/cbtest.c) under PPSSPPHeadless and turn its event-triggered frame dumps
# into PNGs.
#   src/combat/test/shoot-cbtest.sh <cooked_dir> <out_dir> [frames=1400] [emulated_seconds=60]
# cooked_dir: arena_nochars.ashm, arena.pcol, combat.ashcb, knight.ashc/.ashanim, knight_combat.ashanim,
# hollow.ashc/.ashanim, hollow_combat.ashanim, c2232.ashc/.ashanim (tools/cook_*.py). Build first:
#   psp-cmake -B build/cbtest -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build/cbtest --target ash_cbtest
# out_dir gets fr_NNNN.png (combat events), cbtest.log (the program's own lines); the raw dumps, the stage copy and
# the full emulator log are removed (disk). Emulator slot: scripts/emu-slot.sh; RAM arena: the shm shim.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"; repo="$(cd "$here/../../.." && pwd)"
cooked="$(realpath "$1")"; out="$2"; frames="${3:-1400}"; secs="${4:-60}"
eboot="${EBOOT:-$repo/build/cbtest/src/combat/test/EBOOT.PBP}"
stage="$repo/build/stage-cbtest"
mkdir -p "$stage" "$out"
out="$(realpath "$out")"
"$repo/tools/.venv/bin/python" -I - "$stage" <<'PY'
import sys, pathlib
d = pathlib.Path(sys.argv[1]).resolve()
assert "/ash/build/" in str(d) + "/"
for f in d.glob("*"):
    if f.is_file():
        f.unlink()
PY
cp "$eboot" "$stage/EBOOT.PBP"
cp "$cooked/arena_nochars.ashm" "$stage/scene.ashm"
for f in arena.pcol combat.ashcb knight.ashc knight.ashanim knight_combat.ashanim hollow.ashc hollow.ashanim \
         hollow_combat.ashanim c2232.ashc c2232.ashanim; do
  cp "$cooked/$f" "$stage/"
done
echo "$frames" > "$stage/demo.txt"
shim="$repo/build/ppsspp-shm-shim.so"
if [ ! -f "$shim" ]; then
  ( . "$repo/scripts/env.sh"; x86_64-conda-linux-gnu-gcc -shared -fPIC -O2 -o "$shim" "$repo/scripts/ppsspp-shm-shim.c" ) || true
fi
[ -f "$shim" ] && export LD_PRELOAD="$shim"
"$repo/scripts/emu-slot.sh" "$repo/scripts/run-headless.sh" "$stage/EBOOT.PBP" "$secs" "$out/run" || true
unset LD_PRELOAD
grep -a -E 'ash-cbtest' "$out/run/run.log" > "$out/cbtest.log" || true
grep -a -E 'Frame stats|exit code' "$out/run/run.log" | tail -3 >> "$out/cbtest.log" || true
"$repo/tools/.venv/bin/python" -I - "$stage" "$out" <<'PY'
import sys, pathlib
import numpy as np
from PIL import Image
src, dst = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
n = 0
for f in sorted(src.glob("fr_*.raw")):
    a = np.fromfile(f, "<u2")
    if a.size >= 512 * 272:
        a = a[:512 * 272].reshape(272, 512)[:, :480]
        r = ((a & 31) * 255 // 31).astype(np.uint8)
        g = (((a >> 5) & 63) * 255 // 63).astype(np.uint8)
        b = (((a >> 11) & 31) * 255 // 31).astype(np.uint8)
        Image.fromarray(np.dstack([r, g, b])).save(dst / (f.stem + ".png"))
        n += 1
print(f"frames converted: {n}")
# disk: drop the stage copy (assets + raw dumps) and the full emulator log
assert "/ash/build/" in str(src.resolve()) + "/"
for f in src.glob("*"):
    if f.is_file():
        f.unlink()
log = dst / "run" / "run.log"
if log.exists():
    log.unlink()
PY
tail -n 12 "$out/cbtest.log"
