#!/usr/bin/env bash
# Run the menu test EBOOT (src/menu/test) under PPSSPPHeadless, convert dumps: named shots -> <out>/shots/*.png,
# every 3rd frame -> <out>/menu.mp4 + menu.gif. Raw dumps are deleted from Python (inside build/ only).
#   src/menu/test/shoot.sh <out_dir> [dump_every=3] [max_frames=4000] [emulated_seconds=150]
# Needs build/menu/EBOOT.PBP and ~/ash-assets/cooked/menu (tools/cook_menu.py, tools/cook_classes.py) +
# ~/ash-assets/cooked/hall_nochars.ashm. One emulator at a time on the host: runs under flock ~/ash-assets/.emu.lock.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"; repo="$(cd "$here/../../.." && pwd)"
out="$1"; every="${2:-3}"; frames="${3:-4000}"; secs="${4:-150}"
cooked="${ASH_COOKED:-$HOME/ash-assets/cooked}"
stage="$repo/build/stage-menu"
mkdir -p "$stage" "$out/shots"
cp "$repo/build/menu/EBOOT.PBP" "$stage/EBOOT.PBP"
cp "$cooked"/menu/menu.ashui "$cooked"/menu/menu.ashcls "$cooked"/menu/menu_idle.ashanim "$stage/"
cp "$cooked"/menu/cls_*.ashc "$cooked"/menu/head_*.ashc "$stage/"
cp "$cooked/hall_nochars.ashm" "$stage/scene.ashm"
echo "$every $frames" > "$stage/demo.txt"
shim="$repo/build/ppsspp-shm-shim.so"
[ -f "$shim" ] && export LD_PRELOAD="$shim"
flock "$HOME/ash-assets/.emu.lock" "$repo/scripts/run-headless.sh" "$stage/EBOOT.PBP" "$secs" "$out/run" || true
unset LD_PRELOAD
grep -E 'ash-menu' "$out/run/run.log" | grep -v ' shot ' | head -60 || true
# frames of the video: drop the previous run's
"$repo/tools/.venv/bin/python" -I -c "import sys,shutil,pathlib; d=pathlib.Path(sys.argv[1]).resolve(); \
assert d.name=='frames'; shutil.rmtree(d) if d.is_dir() else None" "$out/frames"
"$repo/tools/.venv/bin/python" -I - "$stage" "$out" <<'PY'
import sys
from pathlib import Path
import numpy as np
from PIL import Image
stage, out = Path(sys.argv[1]).resolve(), Path(sys.argv[2])
assert "build" in stage.parts and stage.name == "stage-menu", stage


def conv(f):
    a = np.fromfile(f, "<u2")
    if a.size < 512 * 272:
        return None
    a = a[:512 * 272].reshape(272, 512)[:, :480]
    r = ((a & 31) * 255 // 31).astype(np.uint8)
    g = (((a >> 5) & 63) * 255 // 63).astype(np.uint8)
    b = (((a >> 11) & 31) * 255 // 31).astype(np.uint8)
    return Image.fromarray(np.dstack([r, g, b]))


n = 0
for f in sorted(stage.glob("sh_*.raw")):
    im = conv(f)
    if im is not None:
        im.save(out / "shots" / (f.stem[3:] + ".png"))
        n += 1
    f.unlink()
fr = out / "frames"
fr.mkdir(exist_ok=True)
k = 0
for f in sorted(stage.glob("fr_*.raw")):
    im = conv(f)
    if im is not None:
        im.save(fr / f"f{k:05d}.png")
        k += 1
    f.unlink()
print(f"shots {n}, frames {k}")
# PPSSPPHeadless -l writes ~180 MB of emulator log per run: keep only our lines
log = out / "run" / "run.log"
if log.exists():
    keep = [l for l in log.read_text(errors="replace").splitlines()
            if "ash-menu" in l or "Frame stats" in l or "exit code" in l]
    (out / "run" / "menu.log").write_text("\n".join(keep) + "\n")
    log.unlink()
PY
if command -v ffmpeg >/dev/null && ls "$out"/frames/f*.png >/dev/null 2>&1; then
  fps=$(( 30 / every ))
  nice -n 19 ffmpeg -loglevel error -y -framerate "$fps" -i "$out/frames/f%05d.png" -pix_fmt yuv420p -c:v libx264 -crf 23 "$out/menu.mp4"
  nice -n 19 ffmpeg -loglevel error -y -framerate "$fps" -i "$out/frames/f%05d.png" \
    -vf "fps=8,scale=360:-1:flags=lanczos,split[a][b];[a]palettegen=max_colors=128[p];[b][p]paletteuse=dither=bayer" "$out/menu.gif"
  ls -la "$out"/menu.*
fi
# the PNG frames were only for the video (~150 MB): remove them (disk is nearly full)
"$repo/tools/.venv/bin/python" -I -c "import sys,shutil,pathlib; d=pathlib.Path(sys.argv[1]).resolve(); \
assert d.name=='frames'; shutil.rmtree(d) if d.is_dir() else None" "$out/frames"
