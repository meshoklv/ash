#!/usr/bin/env bash
# Zone draw A/B under PSP-est (the perf-night design notes): src/render/test/wbench.c on fixed cameras.
#   src/render/test/wbench.sh <m18_01|m10_02> <out_dir> <A | B flags_hex vram(0/1)> [frames=3] [secs=120]
#   A = world_draw + world_upload_vram (the game today), B = render_world (RW_* flags, render_world.h)
# Zone pak + .ashmip: ~/ash-assets/cooked/world/zone_m18_01.* or cooked/firelink/zone_m10_02.* (outside git).
# Emulator through scripts/emu-slot.sh + shm shim. Keeps <out>/pspest.log (PSPEST + wb lines), <out>/png/*.png.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"; repo="$(cd "$here/../../.." && pwd)"
zone="$1"; out="$2"; mode="$3"; frames="${4:-3}"; secs="${5:-120}"
ck="${ASH_COOKED:-$HOME/ash-assets/cooked}"; py="$repo/tools/.venv/bin/python"
case "$zone" in m18_01) zd="$ck/world" ;; m10_02) zd="$ck/firelink" ;; *) echo "zone?"; exit 2 ;; esac
tag="$(echo "$mode" | tr -c 'A-Za-z0-9' '_')"
st="$repo/build/render/stage-wb-$zone-$tag"
mkdir -p "$st" "$out/png"
"$py" -I - "$st" <<'PY'
import sys, pathlib
d = pathlib.Path(sys.argv[1])
assert d.name.startswith("stage-wb-") and d.parent.name == "render"
for f in d.glob("wb_*.raw"): f.unlink()
PY
cp "$repo/build/render/pbp_wbench_est/EBOOT.PBP" "$st/"
cp "$zd/zone_$zone.pak" "$st/zone.pak"
[ -f "$zd/zone_$zone.ashmip" ] && cp "$zd/zone_$zone.ashmip" "$st/zone.ashmip"
{ echo "zone zone.pak"; echo "mips zone.ashmip"; echo "mode $mode"; echo "frames $frames"; cat "$here/wb/$zone.txt"; } > "$st/wb.txt"
cp "$repo/src/perf/psp_cost.cfg" "$st/psp_cost.cfg"
printf 'overlay = 0\nmem_probe_every = 0\n%s\n' "${EXTRA_CFG:-}" | tr ';' '\n' >> "$st/psp_cost.cfg"
shim="$repo/build/ppsspp-shm-shim.so"
[ -f "$shim" ] && export LD_PRELOAD="$shim"
( . "$repo/scripts/env.sh"; cd "$st"; "$repo/scripts/emu-slot.sh" "$PPSSPP_HEADLESS" "$st/EBOOT.PBP" --graphics=software \
    --root="$st" --timeout-emulated="$secs" --timeout-wall=$(( secs * 8 + 60 )) 2>&1 | grep -aE '(PSPEST|wb:|render_world)' > "$out/pspest.log" ) || true
unset LD_PRELOAD
"$py" -I - "$st" "$out/png" <<'PY'
import sys, glob, os
import numpy as np
from PIL import Image
src, dst = sys.argv[1], sys.argv[2]
for f in sorted(glob.glob(os.path.join(src, "wb_*.raw"))):
    a = np.fromfile(f, "<u2")
    if a.size >= 512 * 272:
        a = a[:512 * 272].reshape(272, 512)[:, :480]
        r = ((a & 31) * 255 // 31).astype(np.uint8); g = (((a >> 5) & 63) * 255 // 63).astype(np.uint8)
        b = (((a >> 11) & 31) * 255 // 31).astype(np.uint8)
        Image.fromarray(np.dstack([r, g, b])).save(os.path.join(dst, os.path.basename(f)[3:-4] + ".png"))
    os.unlink(f)
PY
grep -aE 'wb: (render_world|world_upload|mode|done)|render_world:' "$out/pspest.log" || true
"$py" -I "$here/wbsum.py" "$out/pspest.log"
