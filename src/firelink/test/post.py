"""src/firelink/test/shoot.sh post-process: raw RGB565 dumps -> PNG stills, contact sheet, mp4, PSP-est table.
    post.py <stage_dir> <out_dir>          post.py --clean <stage_dir>   (deletes *.raw, only build/firelink/stage*)
"""
import glob
import json
import os
import pathlib
import re
import subprocess
import sys

import numpy as np
from PIL import Image, ImageDraw

if len(sys.argv) == 3 and sys.argv[1] == "--clean":
    st = pathlib.Path(sys.argv[2]).resolve()
    root = (pathlib.Path(__file__).resolve().parents[3] / "build" / "firelink").resolve()
    if st.parent == root and st.name.startswith("stage") and st.is_dir():
        for p in st.glob("*.raw"):
            p.unlink()
    else:
        print(f"post: refusing to clean {st}")
    sys.exit(0)

stage, out = sys.argv[1], sys.argv[2]
os.makedirs(out, exist_ok=True)


def rgb(f):
    a = np.fromfile(f, "<u2")[:512 * 272].reshape(272, 512)[:, :480]
    r = ((a & 31) * 255 // 31).astype(np.uint8)
    g = (((a >> 5) & 63) * 255 // 63).astype(np.uint8)
    b = (((a >> 11) & 31) * 255 // 31).astype(np.uint8)
    return np.dstack([r, g, b])


log = open(os.path.join(out, "run", "run.log"), errors="replace").read().splitlines()
fl, est, pending = {}, {}, None
for ln in log:
    m = re.search(r"fl: (\S+\.raw) (.*)", ln)
    if m:
        fl[m.group(1)] = dict(kv.split("=", 1) for kv in m.group(2).split() if "=" in kv)
        pending = m.group(1)
        continue
    m = re.search(r"PSPEST f=\d+ .*", ln)
    if m and pending:
        kv = dict(x.split("=", 1) for x in m.group(0).split()[1:] if "=" in x)
        est[pending] = {k: kv.get(k) for k in ("ge", "cpu", "frame", "fps", "ge333", "frame333", "fps333", "tris",
                                                "draws", "verts", "px", "od", "texpx")}
        pending = None

stills = sorted(glob.glob(os.path.join(stage, "s_*.raw")))
ims = []
table = []
for f in stills:
    nm = os.path.basename(f)[2:-4]
    im = Image.fromarray(rgb(f))
    im.save(os.path.join(out, f"firelink_{nm}.png"))
    k = os.path.basename(f)
    row = dict(name=nm, **{f"fl_{a}": b for a, b in fl.get(k, {}).items()}, **{f"est_{a}": b for a, b in (est.get(k) or {}).items()})
    table.append(row)
    lab = im.copy()
    d = ImageDraw.Draw(lab)
    t = f"{nm} tris {fl.get(k, {}).get('tris', '?')}" + (f" est {est[k]['ge']}ms GE {est[k]['fps']}fps" if k in est else "")
    d.rectangle([0, 0, 8 + 6 * len(t), 13], fill=(0, 0, 0))
    d.text((4, 1), t, fill=(255, 230, 160))
    ims.append(lab)
if ims:
    cols = 2
    rows = (len(ims) + cols - 1) // cols
    sheet = Image.new("RGB", (480 * cols, 272 * rows))
    for i, im in enumerate(ims):
        sheet.paste(im, ((i % cols) * 480, (i // cols) * 272))
    sheet.save(os.path.join(out, "firelink_sheet.png"))
    json.dump(table, open(os.path.join(out, "firelink_stills.json"), "w"), indent=1)
    for r in table:
        print("post:", {k: v for k, v in r.items() if k in ("name", "fl_tris", "fl_world", "fl_far", "fl_sky", "fl_chr",
                                                            "fl_batches", "est_ge", "est_frame", "est_fps", "est_px")})

vids = sorted(glob.glob(os.path.join(stage, "v_*.raw")))
if vids:
    vd = os.path.join(stage, "png")
    os.makedirs(vd, exist_ok=True)
    tris = []
    for i, f in enumerate(vids):
        Image.fromarray(rgb(f)).save(os.path.join(vd, f"f_{i:04d}.png"))
        tris.append(int(fl.get(os.path.basename(f), {}).get("tris", 0)))
    mp4 = os.path.join(out, "firelink_flight.mp4")
    subprocess.run(["nice", "-n", "19", "ffmpeg", "-loglevel", "error", "-y", "-framerate", "30", "-i",
                    os.path.join(vd, "f_%04d.png"), "-pix_fmt", "yuv420p", "-c:v", "libx264", "-crf", "21", mp4],
                   check=False)
    for p in pathlib.Path(vd).glob("f_*.png"):
        p.unlink()
    os.rmdir(vd)
    t = np.array(tris)
    print(f"post: video {len(vids)} frames -> {mp4}; tris per frame min {t.min()} median {int(np.median(t))} "
          f"p95 {int(np.percentile(t, 95))} max {t.max()}")
    json.dump(dict(frames=len(vids), tris_min=int(t.min()), tris_median=int(np.median(t)),
                   tris_p95=int(np.percentile(t, 95)), tris_max=int(t.max())),
              open(os.path.join(out, "firelink_flight.json"), "w"), indent=1)
