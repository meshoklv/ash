#!/usr/bin/env python3
"""Per-camera PSP-est summary of a wbench run (src/render/test/wbench.sh): the last frame of each camera.

    tools/.venv/bin/python src/render/test/wbsum.py <pspest.log> [--json out.json]
Columns: GE ms at 333/166, vertex / pixel stage sums (ms), processed vertices, triangles drawn, draw calls,
texture binds, CLUT bytes, GE command words, CPU ms (emulated x cpu_k: list building + the model's exclusions).
"""
import json
import re
import sys

KV = re.compile(r"(\w+)=([^\s]+)")


def main():
    log = sys.argv[1]
    frames, cams = {}, {}
    for line in open(log, errors="replace"):
        i = line.find("PSPEST f=")
        if i >= 0:
            d = dict(KV.findall(line[i:]))
            frames[int(d["f"])] = d
            continue
        i = line.find("wb: f=")
        if i >= 0:
            d = dict(KV.findall(line[i:]))
            cams[d["cam"]] = int(d["f"])   # last frame of the camera wins
    rows = []
    for cam, f in cams.items():
        d = frames.get(f)
        if not d:
            continue
        g = lambda k: float(d.get(k, 0))
        rows.append({"cam": cam, "ge": g("ge333"), "v": g("ns_v") / 1e6, "p": g("ns_p") / 1e6, "verts": int(g("verts")),
                     "tris": int(g("tris")), "draws": int(g("draws")), "bind": int(g("texbind")), "clutb": int(g("clutb")),
                     "cmds": int(g("cmds")), "cpu": g("cpu333"), "texb_vram": g("texb_vram"), "texb_ram": g("texb_ram"),
                     "minif": g("minif")})
    if not rows:
        raise SystemExit(f"{log}: no frames")
    print(f"{'cam':14s} {'GE ms':>6s} {'vert':>6s} {'pix':>6s} {'verts':>7s} {'tris':>6s} {'draws':>5s} {'bind':>4s} "
          f"{'clutB':>6s} {'cmds':>6s} {'cpu':>5s}")
    for r in rows:
        print(f"{r['cam']:14s} {r['ge']:6.2f} {r['v']:6.2f} {r['p']:6.2f} {r['verts']:7d} {r['tris']:6d} {r['draws']:5d} "
              f"{r['bind']:4d} {r['clutb']:6d} {r['cmds']:6d} {r['cpu']:5.2f}")
    n = len(rows)
    avg = {k: sum(r[k] for r in rows) / n for k in rows[0] if k != "cam"}
    print(f"{'AVG':14s} {avg['ge']:6.2f} {avg['v']:6.2f} {avg['p']:6.2f} {avg['verts']:7.0f} {avg['tris']:6.0f} "
          f"{avg['draws']:5.0f} {avg['bind']:4.0f} {avg['clutb']:6.0f} {avg['cmds']:6.0f} {avg['cpu']:5.2f}")
    if "--json" in sys.argv:
        json.dump({"rows": rows, "avg": avg}, open(sys.argv[sys.argv.index("--json") + 1], "w"), indent=1)


if __name__ == "__main__":
    main()
