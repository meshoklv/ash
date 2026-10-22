#!/usr/bin/env bash
# Save store in PPSSPPHeadless across emulator restarts (src/save/test/test.c):
#   1 write (savedata A seq 1, B seq 2) -> 2 new emulator: read = sample 1 seq 2 -> 3 damage B's DATA.BIN on the
#   memory stick -> 4 new emulator: read falls back to A = sample 0 seq 1 -> 5 file backend only.
#   scripts/test-save-emu.sh [shots_dir]   (screens: <shots_dir>/save-<step>.png; memory stick: build/save-ms)
set -uo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"; repo="$(cd "$here/.." && pwd)"
. "$here/env.sh"
shots="${1:-$HOME/ash-assets/shots/save-$(date +%m%d-%H%M)}"; mkdir -p "$shots"
stage="$repo/build/stage-save"; ms="$repo/build/save-ms"
mkdir -p "$stage" "$ms"
cp "$repo/build/src/save/EBOOT.PBP" "$stage/EBOOT.PBP"
py="$repo/tools/.venv/bin/python"
# clean start: remove our two savedata dirs and the file (Python, only inside build/)
"$py" -I - "$ms" "$stage" <<'PY'
import sys, pathlib, shutil
ms, stage = pathlib.Path(sys.argv[1]).resolve(), pathlib.Path(sys.argv[2]).resolve()
for d in (ms / "PSP/SAVEDATA/ASHDS0001A", ms / "PSP/SAVEDATA/ASHDS0001B"):
    assert "/ash/build/" in str(d)
    if d.is_dir(): shutil.rmtree(d)
for f in ("ash.sav", "ash.sav.tmp"):
    p = stage / f
    if p.exists(): p.unlink()
PY
shim="$repo/build/ppsspp-shm-shim.so"
[ -f "$shim" ] || x86_64-conda-linux-gnu-gcc -shared -fPIC -O2 -o "$shim" "$here/ppsspp-shm-shim.c"
fails=0
step() {   # step <name> <mode.txt contents>
  echo "$2" > "$stage/mode.txt"
  LD_PRELOAD="$shim" "$here/emu-slot.sh" "$PPSSPP_HEADLESS" "$stage/EBOOT.PBP" --graphics=software \
    --root="$stage" --memstick="$ms" --timeout-emulated=6 --timeout-wall=90 \
    --screenshot-save="$shots/save-$1.png" > "$stage/run-$1.log" 2>&1
  echo "== $1 ($2)"; grep 'ash-save' "$stage/run-$1.log"
  grep -q 'ash-save: PASS' "$stage/run-$1.log" || fails=$((fails+1))
}
step 1-write "write"
ls -la "$ms/PSP/SAVEDATA/ASHDS0001A" "$ms/PSP/SAVEDATA/ASHDS0001B" 2>&1
step 2-read "read 1 2"
"$py" -I - "$ms/PSP/SAVEDATA/ASHDS0001B/DATA.BIN" <<'PY'
import sys, pathlib
p = pathlib.Path(sys.argv[1]); b = bytearray(p.read_bytes()); i = len(b) // 2; b[i] ^= 0x40; p.write_bytes(bytes(b))
print(f"damaged {p.name} of B: byte {i} of {len(b)}")
PY
step 3-read-damaged-B "read 0 1"
step 4-file "file"
ls -la "$stage"/ash.sav* 2>&1
echo "fails: $fails  shots: $shots"
[ $fails -eq 0 ] && echo "ALL PASS" || echo "SOME FAIL"
