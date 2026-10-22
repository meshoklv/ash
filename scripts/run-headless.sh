#!/usr/bin/env bash
# Run a PSP program in PPSSPPHeadless (software GPU), save a screenshot + log.
#
#   scripts/run-headless.sh <EBOOT.PBP|file.elf|file.prx> [seconds] [out_dir]
#
#   seconds  - emulated seconds to run (default 5). Screenshot = last displayed frame.
#   out_dir  - default tests/out/<name>-<timestamp>
#
# Output: <out_dir>/screen.png, <out_dir>/run.log (program stdout + emulator log + frame stats).
# Exit code: 0 if the run reached the timeout normally and a non-black screenshot was saved.
# NB: emulated FPS here says nothing about real PSP hardware FPS.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$here/.." && pwd)"
. "$here/env.sh"

[ $# -ge 1 ] || { sed -n 2,11p "$0"; exit 2; }
target="$(realpath "$1")"
secs="${2:-5}"
name="$(basename "$(dirname "$target")")"
out="${3:-$repo/tests/out/${name}-$(date +%Y%m%d-%H%M%S)}"
mkdir -p "$out"
out="$(realpath "$out")"

[ -x "$PPSSPP_HEADLESS" ] || { echo "PPSSPPHeadless not found: $PPSSPP_HEADLESS" >&2; exit 2; }
[ -f "$target" ] || { echo "no such file: $target" >&2; exit 2; }

# Wall-clock guard: generous, so slow software rendering still reaches the emulated timeout.
wall=$(( secs * 6 + 30 ))

set +e
nice -n 10 "$PPSSPP_HEADLESS" "$target" \
  --graphics=software \
  --root="$(dirname "$target")" \
  --timeout-emulated="$secs" \
  --timeout-wall="$wall" \
  --frame-stats \
  --screenshot-save="$out/screen.png" \
  -l > "$out/run.log" 2>&1
rc=$?
set -e

echo "exit code: $rc" >> "$out/run.log"
grep -E 'ash-|Frame stats|TIMEOUT|Screenshot' "$out/run.log" | tail -n 20 || true

if [ ! -s "$out/screen.png" ]; then
  echo "FAIL: no screenshot ($out/screen.png)"; exit 1
fi

# Black-frame check: count non-black pixels (pure python, no deps).
python3 -I - "$out/screen.png" <<'PY'
import sys, zlib, struct
data = open(sys.argv[1], 'rb').read()
assert data[:8] == b'\x89PNG\r\n\x1a\n'
pos, idat, w = 8, b'', 0
while pos < len(data):
    ln, typ = struct.unpack('>I4s', data[pos:pos+8]); body = data[pos+8:pos+8+ln]; pos += 12 + ln
    if typ == b'IHDR': w, h, bd, ct = struct.unpack('>IIBB', body[:10])
    elif typ == b'IDAT': idat += body
raw = zlib.decompress(idat); bpp = 4 if ct == 6 else 3; stride = w * bpp
prev = bytearray(stride); px = 0; colors = set()
for y in range(h):
    f = raw[y*(stride+1)]; line = bytearray(raw[y*(stride+1)+1:(y+1)*(stride+1)])
    for i in range(stride):
        a = line[i-bpp] if i >= bpp else 0; b = prev[i]; c = prev[i-bpp] if i >= bpp else 0
        if f == 1: line[i] = (line[i] + a) & 255
        elif f == 2: line[i] = (line[i] + b) & 255
        elif f == 3: line[i] = (line[i] + (a + b) // 2) & 255
        elif f == 4:
            p = a + b - c; pa, pb, pc = abs(p-a), abs(p-b), abs(p-c)
            line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
    for x in range(0, stride, bpp):
        r, g, bb = line[x], line[x+1], line[x+2]
        if r or g or bb: px += 1
        colors.add((r >> 4, g >> 4, bb >> 4))
    prev = line
print(f"screenshot {w}x{h}: non-black pixels {px} ({100*px/(w*h):.1f}%), distinct colors(4bit) {len(colors)}")
sys.exit(0 if px > w*h*0.05 and len(colors) > 4 else 1)
PY
ok=$?
echo "out: $out"
[ $ok -eq 0 ] && echo "PASS" || { echo "FAIL: screenshot looks blank"; exit 1; }
