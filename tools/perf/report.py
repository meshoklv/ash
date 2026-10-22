#!/usr/bin/env python3
"""Summarise PSP-est lines of a PPSSPPHeadless run.log (src/perf, docs/psp-cost-model.md).

    tools/perf/report.py <run.log> [--skip 3] [--json out.json] [--label name]
Prints: frames, PSP-est FPS at 333 MHz (vsync-quantised, as the game waits for vblank), frame / CPU / GE ms
(avg, p95, worst), what drives the GE time, budget warnings and the memory low-water mark.
The plan §4.5 criterion is evaluated on the estimate: avg FPS >= 29.5, p95 <= 34 ms, frames > 50 ms <= 1 per 300.
"""
import argparse
import json
import re
import statistics

KV = re.compile(r"(\w+)=([^\s]+)")


def num(v):
    try:
        return float(v)
    except ValueError:
        return v


def pct(xs, p):
    if not xs:
        return 0.0
    s = sorted(xs)
    return s[min(len(s) - 1, int(len(s) * p / 100))]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("--skip", type=int, default=3, help="first frames to ignore (loading)")
    ap.add_argument("--json")
    ap.add_argument("--label", default="")
    a = ap.parse_args()

    frames, warns, mem, init = [], {}, [], []
    for line in open(a.log, errors="replace"):
        i = line.find("PSPEST")
        if i < 0:
            continue
        line = line[i:].strip()
        if line.startswith("PSPEST f="):
            d = {k: num(v) for k, v in KV.findall(line)}
            if d["f"] >= a.skip:
                frames.append(d)
        elif line.startswith("PSPEST-WARN"):
            d = dict(KV.findall(line))
            w = warns.setdefault(d["kind"], {"count": 0, "max": 0.0, "budget": d.get("budget")})
            w["count"] = max(w["count"], int(d.get("count", 1)))
            w["max"] = max(w["max"], float(d.get("value", 0)))
        elif line.startswith("PSPEST-MEM"):
            mem.append({k: num(v) for k, v in KV.findall(line)})
        elif line.startswith("PSPEST-INIT"):
            init.append(line)
    if not frames:
        raise SystemExit(f"{a.log}: no PSPEST frame lines")

    def col(k):
        return [f[k] for f in frames if isinstance(f.get(k), float)]

    f333, c333, g333 = col("frame333"), col("cpu333"), col("ge333")
    fps333 = col("fps333")
    n = len(frames)
    avg_fps = 1000.0 / statistics.mean(f333) if statistics.mean(f333) > 0 else 0
    vs_fps = statistics.mean(fps333)
    over50 = sum(1 for x in f333 if x > 50)
    s = {
        "label": a.label, "frames": n,
        "mhz_as_run": f"{frames[0]['mhz']}",
        "fps333_vsync_avg": round(vs_fps, 1),
        "fps333_unlocked": round(avg_fps, 1),
        "share_frames_30fps": round(sum(1 for x in fps333 if x >= 29.9) / n, 3),
        "frame333_ms": {"avg": round(statistics.mean(f333), 2), "p95": round(pct(f333, 95), 2), "max": round(max(f333), 2)},
        "cpu333_ms": {"avg": round(statistics.mean(c333), 2), "p95": round(pct(c333, 95), 2), "max": round(max(c333), 2)},
        "ge333_ms": {"avg": round(statistics.mean(g333), 2), "p95": round(pct(g333, 95), 2), "max": round(max(g333), 2)},
        "as_run": {"fps_vsync_avg": round(statistics.mean(col("fps")), 1),
                   "frame_ms_avg": round(statistics.mean(col("frame")), 2)},
        "emu_cpu_ms_avg": round(statistics.mean(col("emu_cpu")), 2),
        "worst_frame": max(frames, key=lambda f: f["frame333"])["f"],
    }
    keys = ["tris", "culled", "clipped", "draws", "skinned", "verts", "vf32", "vs16", "vskin", "vlit", "vbytes", "px", "od",
            "texpx", "texpx_ram", "texb_ram", "texb_vram", "minif", "texbind", "clut", "bonemats", "cmds"]
    s["avg"] = {k: round(statistics.mean(col(k)), 2) for k in keys if col(k)}
    ns = {k: statistics.mean(col(k)) / 1e6 for k in ("ns_v", "ns_p", "ns_s", "ns_c")}
    s["ge_parts_ms_serial"] = {"vertex": round(ns["ns_v"], 2), "pixel": round(ns["ns_p"], 2),
                               "state+prim": round(ns["ns_s"], 2), "commands+lists": round(ns["ns_c"], 2)}
    # plan §4.5 on the estimate
    s["plan_4_5"] = {"avg_fps_ge_29_5": vs_fps >= 29.5, "p95_le_34ms": pct(f333, 95) <= 34.0,
                     "frames_over_50ms_per_300": round(over50 * 300.0 / n, 2)}
    s["warnings"] = warns
    if mem:
        s["mem_min_free_hw_kb"] = min(m["heap_free_hw_kb"] for m in mem)
    s["init"] = init

    print(f"== PSP-est {a.label} ({a.log}) ==")
    print(f"frames {n}, program clock {s['mhz_as_run']} MHz (estimates below are at 333/166)")
    print(f"PSP-est FPS @333 (vsync 60/n): avg {s['fps333_vsync_avg']}, 30-fps frames {s['share_frames_30fps']*100:.0f}%,"
          f" unlocked {s['fps333_unlocked']}")
    for k in ("frame333_ms", "cpu333_ms", "ge333_ms"):
        v = s[k]
        print(f"  {k:12s} avg {v['avg']:6.2f}  p95 {v['p95']:6.2f}  worst {v['max']:6.2f}")
    print(f"  as built ({s['mhz_as_run']} MHz): fps {s['as_run']['fps_vsync_avg']}, frame {s['as_run']['frame_ms_avg']} ms")
    print(f"  emulated CPU (PPSSPP) {s['emu_cpu_ms_avg']} ms/frame; worst frame #{int(s['worst_frame'])}")
    print("GE parts (serial sums, ms): " + ", ".join(f"{k} {v}" for k, v in s["ge_parts_ms_serial"].items()))
    print("per frame avg: " + ", ".join(f"{k} {v:g}" for k, v in s["avg"].items()))
    print(f"plan §4.5 on the estimate: {s['plan_4_5']}")
    for k, w in sorted(warns.items()):
        print(f"WARN {k}: {w['count']} frames, max {w['max']:g} (budget {w['budget']})")
    if mem:
        print(f"memory: min free heap on hw (est) {s['mem_min_free_hw_kb']:.0f} KB")
    if a.json:
        json.dump(s, open(a.json, "w"), indent=1, ensure_ascii=False)


if __name__ == "__main__":
    main()
