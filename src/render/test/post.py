"""Post-process of src/render/test/shoot.sh: raw RGB565 dumps -> PNG, before|after stills, coverage counts,
before|after mp4, cost table (render counters + PSPEST lines of src/perf when the _est EBOOT ran).
    post.py <stage_dir> <out_dir> <scene>
"""
import glob
import json
import os
import re
import subprocess
import sys

import numpy as np
from PIL import Image, ImageDraw

if len(sys.argv) == 3 and sys.argv[1] == "--clean":
    # delete *.raw dumps of a stage dir; only inside <repo>/build/render/stage-*
    import pathlib
    st = pathlib.Path(sys.argv[2]).resolve()
    root = (pathlib.Path(__file__).resolve().parents[3] / "build" / "render").resolve()
    if st.parent == root and st.name.startswith("stage-") and st.is_dir():
        for p in st.glob("*.raw"):
            p.unlink()
    else:
        print(f"post: refusing to clean {st}")
    sys.exit(0)

stage, out, scene = sys.argv[1], sys.argv[2], sys.argv[3]
os.makedirs(os.path.join(out, "frames"), exist_ok=True)


def load(f):
    a = np.fromfile(f, "<u2")[:512 * 272].reshape(272, 512)[:, :480]
    return a


def rgb(a):
    r = ((a & 31) * 255 // 31).astype(np.uint8)
    g = (((a >> 5) & 63) * 255 // 63).astype(np.uint8)
    b = (((a >> 11) & 31) * 255 // 31).astype(np.uint8)
    return np.dstack([r, g, b])


def label(im, text):
    d = ImageDraw.Draw(im)
    d.rectangle([0, 0, 8 + 7 * len(text), 14], fill=(0, 0, 0))
    d.text((4, 2), text, fill=(255, 230, 160))
    return im


log = open(os.path.join(out, "run", "run.log"), errors="replace").read().splitlines()
rt = {}
pending = None
est = {}
for ln in log:
    m = re.search(r"rtest: (\S+\.raw) (.*)", ln)
    if m:
        rt[m.group(1)] = dict(kv.split("=") for kv in m.group(2).split())
        pending = m.group(1)
        continue
    m = re.search(r"PSPEST f=\d+ .*", ln)
    if m and pending:
        kv = dict(x.split("=", 1) for x in m.group(0).split()[1:] if "=" in x)
        est[pending] = {k: kv.get(k) for k in ("ge", "cpu", "frame", "fps", "ge333", "frame333", "fps333", "tris",
                                                "draws", "verts", "vskin", "px", "od", "blendpx", "texpx")}
        pending = None

cams = sorted({re.match(r"c(\d)_", os.path.basename(f)).group(1) for f in glob.glob(os.path.join(stage, "c*_*.raw"))})
table = []
for c in cams:
    fb, fa, fc = (os.path.join(stage, f"c{c}_{m}.raw") for m in ("before", "after", "cover"))
    if not (os.path.exists(fb) and os.path.exists(fa)):
        continue
    b, a = rgb(load(fb)), rgb(load(fa))
    Image.fromarray(b).save(os.path.join(out, f"{scene}_c{c}_before.png"))
    Image.fromarray(a).save(os.path.join(out, f"{scene}_c{c}_after.png"))
    sbs = Image.new("RGB", (960, 272))
    sbs.paste(label(Image.fromarray(b), "BEFORE"), (0, 0))
    sbs.paste(label(Image.fromarray(a), "AFTER"), (480, 0))
    sbs.save(os.path.join(out, f"{scene}_c{c}_sbs.png"))
    row = dict(cam=int(c), before=rt.get(f"c{c}_before.raw"), after=rt.get(f"c{c}_after.raw"),
               est_before=est.get(f"c{c}_before.raw"), est_after=est.get(f"c{c}_after.raw"))
    names = {1: "fog", 2: "mips", 4: "lightpass", 8: "charlight", 16: "rim", 32: "shadows", 64: "fx", 128: "vlit"}
    eb0 = est.get(f"c{c}_before.raw") or {}
    row["only"] = {}
    for bit, nm in names.items():
        e = est.get(f"c{c}_only{bit:02x}.raw")
        if e and eb0.get("ge"):
            row["only"][nm] = dict(ge_ms=round(float(e["ge"]) - float(eb0["ge"]), 2),
                                   frame_ms=round(float(e["frame"]) - float(eb0["frame"]), 2),
                                   px=round(float(e["px"]) - float(eb0["px"])), tris=int(e["tris"]) - int(eb0["tris"]))
            if bit in (2, 4, 128):
                Image.fromarray(rgb(load(os.path.join(stage, f"c{c}_only{bit:02x}.raw")))).save(
                    os.path.join(out, "frames", f"{scene}_c{c}_only_{nm}.png"))
    if os.path.exists(fc):
        cv = load(fc)
        mag = int(((cv == 0xFFFF) & (load(fa) != 0xFFFF)).sum())   # passes drawn with logic op SET = white
        row["cover_px"] = mag
        row["cover_frac"] = round(mag / (480 * 272), 4)
        Image.fromarray(rgb(cv)).save(os.path.join(out, "frames", f"{scene}_c{c}_cover.png"))
    table.append(row)
json.dump(table, open(os.path.join(out, f"{scene}_costs.json"), "w"), indent=1)
for r in table:
    eb, ea = r.get("est_before") or {}, r.get("est_after") or {}
    print(f"post: {scene} cam{r['cam']}: cover {r.get('cover_frac')} | after: {r['after']} | "
          f"PSP-est GE before {eb.get('ge')} ms -> after {ea.get('ge')} ms, frame {eb.get('frame')} -> {ea.get('frame')}")
    print("      only: " + ", ".join(f"{k} {v['ge_ms']:+.2f}ms/{v['tris']:+d}tri/{v['px']:+d}px" for k, v in r["only"].items()))

# video: before | after side by side
vb = sorted(glob.glob(os.path.join(stage, "v_before_*.raw")))
va = sorted(glob.glob(os.path.join(stage, "v_after_*.raw")))
n = min(len(vb), len(va))
if n:
    vd = os.path.join(out, "frames", "video")
    os.makedirs(vd, exist_ok=True)
    for i in range(n):
        sbs = Image.new("RGB", (960, 272))
        sbs.paste(label(Image.fromarray(rgb(load(vb[i]))), "BEFORE"), (0, 0))
        sbs.paste(label(Image.fromarray(rgb(load(va[i]))), "AFTER (src/render)"), (480, 0))
        sbs.save(os.path.join(vd, f"f_{i:04d}.png"))
    mp4 = os.path.join(out, f"{scene}_before_after.mp4")
    subprocess.run(["nice", "-n", "19", "ffmpeg", "-loglevel", "error", "-y", "-framerate", "30", "-i",
                    os.path.join(vd, "f_%04d.png"), "-pix_fmt", "yuv420p", "-c:v", "libx264", "-crf", "20", mp4],
                   check=False)
    print(f"post: video {n} frames -> {mp4}")
