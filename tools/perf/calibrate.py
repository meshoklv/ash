#!/usr/bin/env python3
"""Calibrate the PSP-est cost model (src/perf/psp_cost.cfg) against the hardware bench (src/bench -> results.txt).

How it works (docs/psp-cost-model.md "Calibration"):
  1. `features`: the bench is run once in PPSSPPHeadless linked with src/perf and log_draws=1
     (scripts/psp-est.sh bench <out>). Every GE list the bench times is walked by the same code that runs in the
     game, so for every bench line ("tri.clut4.idx.z.10k", "fill.flat.x2", "open.10k.clut4.od2", ...) we know the
     model inputs per draw: vertices, bytes, weights, lighting, triangles, covered pixels, Z/blend, texel bytes.
     The emulator's own bench numbers (CPU section, free memory) are kept too.
  2. `fit`: reads results.txt from a real PSP (same bench, same labels), fits the GE coefficients by non-linear
     least squares on log(time) (scipy, bounds >= 0, a weak prior towards the current values so that coefficients
     the bench cannot see stay put), computes cpu_k = hardware / emulator time of the CPU tests, and the memory
     difference. Prints measured vs predicted before/after; --write updates psp_cost.cfg in place (comments kept).
  3. `selftest`: fakes a "hardware" results.txt from perturbed coefficients and checks that `fit` recovers them.

    tools/perf/calibrate.py features <bench run.log> --out tools/perf/bench_features.json
    tools/perf/calibrate.py fit --results results.txt [--features tools/perf/bench_features.json] [--write]
        (CPU / memory baseline: tools/perf/bench_emu_results.txt = the plain bench in PPSSPPHeadless)
    tools/perf/calibrate.py selftest [--features ...]
"""
import argparse
import datetime
import json
import math
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
CFG = os.path.join(REPO, "src", "perf", "psp_cost.cfg")
FEAT = os.path.join(HERE, "bench_features.json")
KV = re.compile(r"(\w+)=([-0-9.]+)")
RES = re.compile(r"^([A-Za-z0-9_.@/+ ()]+?):\s+(?:avg\s+)?(-?[0-9.]+)\s*ms")

# coefficients the bench can identify (others stay at their cfg value)
FIT = ["ge_vert_ns", "ge_vert_byte_ns", "ge_tri_ns", "ge_pix_ns", "ge_pix_zread_ns", "ge_pix_blend_ns", "ge_tex_px_ns",
       "ge_tex_ram_byte_ns", "ge_tex_vram_byte_ns", "ge_prim_ns", "ge_overlap", "ge_weight_ns", "ge_light_ns",
       "ge_list_ns"]


# ------------------------------------------------------------------ cfg
def cfg_read(path=CFG):
    kv = {}
    for raw in open(path, encoding="utf-8"):
        line = raw.split("#", 1)[0].strip()
        m = re.match(r"^([a-z0-9_]+)\s*=\s*([-+0-9.eE]+)$", line)
        if m:
            kv[m.group(1)] = float(m.group(2))
    return kv


def cfg_write(path, new, note):
    out = []
    for raw in open(path, encoding="utf-8"):
        m = re.match(r"^([a-z0-9_]+)(\s*=\s*)([-+0-9.eE]+)(.*)$", raw.rstrip("\n"))
        if m and m.group(1) in new:
            v = new[m.group(1)]
            val = f"{v:.4g}" if not float(v).is_integer() else f"{int(v)}"
            tail = m.group(4)
            if m.group(1) != "calibrated" and "[fit" not in tail:
                tail = tail.replace("#", f"# [fit {note}] was {m.group(3)};", 1) if "#" in tail else f"   # [fit {note}]"
            out.append(f"{m.group(1)}{m.group(2)}{val}{tail}")
        else:
            out.append(raw.rstrip("\n"))
    open(path, "w", encoding="utf-8").write("\n".join(out) + "\n")


# ------------------------------------------------------------------ the model (mirror of src/perf/ge_scan.c)
def draw_ns(d, c):
    lit = d["lit"]
    tv = d["cnt"] * (c["ge_vert_ns"] + d["nw"] * c["ge_weight_ns"] + (c["ge_light_ns"] + d["nl"] * c["ge_light_each_ns"] if lit else 0)) \
        + d["cnt"] * (d["vsz"] + d["isz"]) * c["ge_vert_byte_ns"] + d["tris"] * c["ge_tri_ns"]
    fbm = c["ge_pix_32bit_mul"] if d["fb32"] else 1.0
    if d["clear"]:
        tp = d["px"] * c["ge_clear_px_ns"] * fbm
    else:
        # zread/zwrite are fitted as one number (the bench toggles both together): zwrite follows zread
        zw = c["ge_pix_zread_ns"] * (c["_zw_ratio"])
        tp = d["px"] * c["ge_pix_ns"] * fbm + d["zr"] * c["ge_pix_zread_ns"] + d["zw"] * zw + d["bl"] * c["ge_pix_blend_ns"] \
            + d["texpx"] * c["ge_tex_px_ns"] + d["tbr"] * c["ge_tex_ram_byte_ns"] * c["ge_tex_ram_contention"] \
            + d["tbv"] * c["ge_tex_vram_byte_ns"]
    big, small = max(tv, tp), min(tv, tp)
    binds = d["bind"] / max(c.get("_bind_ns_logged", c["ge_tex_bind_ns"]), 1e-9)
    return c["ge_prim_ns"] + big + c["ge_overlap"] * small + binds * c["ge_tex_bind_ns"]


def list_ms(lst, c):
    ns = c["ge_list_ns"] + lst["cmds"] * c["ge_cmd_ns"] + sum(draw_ns(d, c) for d in lst["draws"])
    return ns / 1e6


# ------------------------------------------------------------------ features
def parse_results(path):
    """bench result lines (results.txt, or 'ash-bench: ' lines of an emulator log) -> {label: ms or dict}"""
    out = {}
    for raw in open(path, errors="replace"):
        line = raw.strip()
        if "ash-bench: " in line:
            line = line.split("ash-bench: ", 1)[1]
        m = re.match(r"^cpu@(\d+)/\d+: matmul fpu=([0-9.]+)ns vfpu=([0-9.]+)ns pose32=([0-9.]+)us loop=([0-9.]+)ms", line)
        if m:
            out[f"cpu@{m.group(1)}"] = {"matmul_fpu": float(m.group(2)), "matmul_vfpu": float(m.group(3)),
                                        "pose32": float(m.group(4)), "loop": float(m.group(5))}
            continue
        m = re.match(r"^mem\.maxfree_kb=(\d+)", line)
        if m:
            out["mem.maxfree_kb"] = int(m.group(1))
            continue
        m = re.match(r"^(skin\.[^:]+): avg ([0-9.]+) ms .*\| cpu ([0-9.]+) build ([0-9.]+) wait ([0-9.]+)", line)
        if m:
            out[m.group(1)] = {"avg": float(m.group(2)), "cpu": float(m.group(3)), "build": float(m.group(4)),
                               "wait": float(m.group(5))}
            continue
        m = RES.match(line)
        if m and (line.startswith("tri.") or line.startswith("fill.") or line.startswith("open.")):
            out[m.group(1).strip()] = float(m.group(2))
    return out


def cmd_features(a):
    tests, cur_lists, lists = {}, [], {}
    for raw in open(a.log, errors="replace"):
        i = raw.find("PSPEST-")
        if i >= 0:
            line = raw[i:].strip()
            d = {k: float(v) for k, v in KV.findall(line)}
            if line.startswith("PSPEST-D"):
                lists.setdefault(int(d["l"]), {"draws": [], "cmds": 0})["draws"].append(d)
            elif line.startswith("PSPEST-L"):
                l = lists.setdefault(int(d["l"]), {"draws": [], "cmds": 0})
                l["cmds"] = d["cmds"]
                cur_lists.append(l)
            continue
        if "ash-bench: " not in raw:
            continue
        line = raw.split("ash-bench: ", 1)[1].strip()
        key = line.split(":", 1)[0].strip()
        if line.startswith(("tri.", "fill.", "open.")):
            timed = [l for l in cur_lists if l["draws"] and not all(d["clear"] for d in l["draws"])]
            if timed:
                tests[key] = {"kind": "ge", "list": timed[-1]}
        elif line.startswith("skin."):
            frames = [l for l in cur_lists if any(not d["clear"] for d in l["draws"])]
            if frames:
                tests[key] = {"kind": "frame", "list": frames[-1]}
        if line.startswith(("tri.", "fill.", "open.", "skin.")):
            cur_lists = []
    emu = parse_results(a.log)
    cfg = cfg_read(a.cfg)
    out = {"made": datetime.date.today().isoformat(), "source_log": os.path.abspath(a.log),
           "cfg_at_extraction": {k: cfg[k] for k in ("ge_tex_bind_ns", "ge_tex_unswizzled_mul", "ge_tex_line_bytes",
                                                     "ge_tex_bilinear_mul", "ge_tex_ratio_min")},
           "tests": tests, "emulator_results": emu}
    json.dump(out, open(a.out, "w"), indent=0)
    print(f"features: {len(tests)} bench tests -> {a.out}")
    for k, t in tests.items():
        print(f"  {k:45s} draws {len(t['list']['draws']):3d}  model {list_ms(t['list'], coefs(cfg, out)):7.2f} ms  emu {emu.get(k, '')}")


def coefs(cfg, feat):
    c = dict(cfg)
    c["_bind_ns_logged"] = feat["cfg_at_extraction"]["ge_tex_bind_ns"]
    c["_zw_ratio"] = cfg["ge_pix_zwrite_ns"] / cfg["ge_pix_zread_ns"] if cfg["ge_pix_zread_ns"] > 0 else 1.0
    return c


# ------------------------------------------------------------------ fit
def fit_ge(feat, hw, cfg, prior_w=0.05, verbose=True):
    import numpy as np
    from scipy.optimize import least_squares
    names = [k for k in FIT if k in cfg]
    data = []
    for k, t in feat["tests"].items():
        v = hw.get(k)
        if t["kind"] == "ge" and isinstance(v, float) and v > 0:
            data.append((k, t["list"], v))
        elif t["kind"] == "frame" and isinstance(v, dict) and v["build"] + v["wait"] > 0:
            # skinning loop: the GE starts on the first draw while the CPU still builds -> GE busy ~ build + wait
            data.append((k, t["list"], v["build"] + v["wait"]))
    if len(data) < 4:
        raise SystemExit(f"only {len(data)} GE tests with hardware numbers - need >= 4")
    base = coefs(cfg, feat)
    x0 = np.array([max(cfg[n], 1e-3) for n in names])

    def unpack(x):
        c = dict(base)
        for n, v in zip(names, x):
            c[n] = v
        return c

    def resid(lx):
        x = np.exp(lx)
        c = unpack(x)
        r = [math.log(max(list_ms(l, c), 1e-6) / ms) for _, l, ms in data]
        r += list(prior_w * (lx - np.log(x0)))
        return r

    sol = least_squares(resid, np.log(x0), method="trf")
    c = unpack(np.exp(sol.x))
    rows = []
    for k, l, ms in data:
        rows.append((k, ms, list_ms(l, base), list_ms(l, c)))
    if verbose:
        print(f"{'bench test':45s} {'hw ms':>8s} {'before':>8s} {'after':>8s}")
        for k, ms, b, af in rows:
            print(f"{k:45s} {ms:8.2f} {b:8.2f} {af:8.2f}  ({100*(af/ms-1):+.0f}%)")
        errb = [abs(b / ms - 1) for _, ms, b, _ in rows]
        erra = [abs(af / ms - 1) for _, ms, _, af in rows]
        print(f"mean |error|: before {100*sum(errb)/len(errb):.0f}%, after {100*sum(erra)/len(erra):.0f}% "
              f"(max {100*max(erra):.0f}%) on {len(rows)} tests")
    new = {n: float(c[n]) for n in names}
    new["ge_pix_zwrite_ns"] = float(c["ge_pix_zread_ns"] * base["_zw_ratio"])
    return new, rows


def fit_cpu(emu, hw):
    ratios = {}
    e, h = emu.get("cpu@333"), hw.get("cpu@333")
    if isinstance(e, dict) and isinstance(h, dict):
        for k in ("loop", "matmul_fpu", "matmul_vfpu", "pose32"):
            if e.get(k, 0) > 0 and h.get(k, 0) > 0:
                ratios[k] = h[k] / e[k]
    if not ratios:
        return None, ratios
    # gameplay code is closest to pose32 (FPU, memory) and loop (integer): weight them double
    w = {"loop": 2, "pose32": 2, "matmul_fpu": 1, "matmul_vfpu": 1}
    lg = sum(w[k] * math.log(v) for k, v in ratios.items()) / sum(w[k] for k in ratios)
    return math.exp(lg), ratios


def cmd_fit(a):
    feat = json.load(open(a.features))
    hw = parse_results(a.results)
    cfg = cfg_read(a.cfg)
    print(f"hardware results: {a.results} ({len(hw)} lines parsed); features: {a.features}")
    new, rows = fit_ge(feat, hw, cfg)
    emu = parse_results(a.emu_results) if os.path.exists(a.emu_results) else feat["emulator_results"]
    print(f"emulator baseline (CPU, memory): {a.emu_results if os.path.exists(a.emu_results) else 'features file'}")
    k, ratios = fit_cpu(emu, hw)
    if k:
        print("CPU hw/emulator ratios @333: " + ", ".join(f"{n} {v:.2f}" for n, v in ratios.items()) + f" -> cpu_k {k:.2f}")
        new["cpu_k"] = round(k, 3)
    else:
        print("CPU: no cpu@333 line in both emulator features and results -> cpu_k unchanged")
    em, hm = emu.get("mem.maxfree_kb"), hw.get("mem.maxfree_kb")
    if em and hm:
        new["mem_hw_minus_emu_kb"] = max(0, em - hm)
        print(f"memory: emulator maxfree {em} KB, hardware {hm} KB -> mem_hw_minus_emu_kb {new['mem_hw_minus_emu_kb']}")
    new["calibrated"] = 1
    print("new coefficients: " + ", ".join(f"{n}={v:.4g}" for n, v in new.items()))
    if a.write:
        cfg_write(a.cfg, new, f"{datetime.date.today().isoformat()} {os.path.basename(a.results)}")
        print(f"written: {a.cfg}  (rebuild, or copy it next to the EBOOT as psp_cost.cfg)")


def cmd_selftest(a):
    import random
    feat = json.load(open(a.features))
    cfg = cfg_read(a.cfg)
    random.seed(1)
    truth = dict(cfg)
    for n in FIT:
        if n in truth:
            truth[n] = cfg[n] * random.uniform(0.4, 2.5)
    truth["ge_pix_zwrite_ns"] = truth["ge_pix_zread_ns"] * cfg["ge_pix_zwrite_ns"] / cfg["ge_pix_zread_ns"]
    ct = coefs(truth, feat)
    ct["_zw_ratio"] = cfg["ge_pix_zwrite_ns"] / cfg["ge_pix_zread_ns"]
    fake = {}
    for k, t in feat["tests"].items():
        ms = list_ms(t["list"], ct) * random.uniform(0.97, 1.03)
        fake[k] = ms if t["kind"] == "ge" else {"avg": ms, "cpu": 0.0, "build": 0.1, "wait": ms - 0.1}
    new, rows = fit_ge(feat, fake, cfg, verbose=False)
    erra = [abs(af / ms - 1) for _, ms, _, af in rows]
    errb = [abs(b / ms - 1) for _, ms, b, _ in rows]
    print(f"selftest: {len(rows)} fake hardware tests (truth = cfg x U(0.4,2.5), 3% noise)")
    print(f"  prediction error before fit {100*sum(errb)/len(errb):.0f}%, after {100*sum(erra)/len(erra):.1f}% (max {100*max(erra):.1f}%)")
    for n in FIT:
        if n in new:
            print(f"  {n:22s} cfg {cfg[n]:8.3f} truth {truth[n]:8.3f} fit {new[n]:8.3f}  ({100*(new[n]/truth[n]-1):+.0f}%)")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("features")
    p.add_argument("log")
    p.add_argument("--out", default=FEAT)
    p.add_argument("--cfg", default=CFG)
    p = sub.add_parser("fit")
    p.add_argument("--results", required=True)
    p.add_argument("--features", default=FEAT)
    p.add_argument("--cfg", default=CFG)
    p.add_argument("--write", action="store_true")
    p.add_argument("--emu-results", default=os.path.join(HERE, "bench_emu_results.txt"),
                   help="plain bench (no src/perf) run in PPSSPPHeadless: CPU + memory baseline")
    p = sub.add_parser("selftest")
    p.add_argument("--features", default=FEAT)
    p.add_argument("--cfg", default=CFG)
    a = ap.parse_args()
    {"features": cmd_features, "fit": cmd_fit, "selftest": cmd_selftest}[a.cmd](a)


if __name__ == "__main__":
    sys.exit(main())
