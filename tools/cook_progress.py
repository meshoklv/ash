#!/usr/bin/env python3
"""Cook the knight's progression clips (Estus, bonfire rest, death) into `knight_prg.ashanim` for src/progress/.

    tools/.venv/bin/python tools/cook_progress.py --out-dir ~/ash-assets/cooked [--merge-twist]

Same skeleton, quantisation and file format as tools/cook_knight.py (imported, not copied): the clips can be
sampled with the knight's AnimSet bones; the game loads the file as a second AnimSet (bone counts must match -
pass --merge-twist iff the knight was cooked with it).

Clip ids - from the player's EzState c0000.esd (state machine 0/1, command ChangeGeneralAnimCategorized),
checked with tools/ai/esd_calls.py-style dump (docs/progress-ui.md):
  Estus (EquipParamGoods 201 UseAnimation 10): state 16 a00_7585 (raise) -> 18 a00_7586 (drink, repeated while
  GetItemAnimType == 10) -> 19 a00_7587 (lower); empty flask (UseAnimation 254): 17 a00_7588.        [проверено]
  Bonfire rest: a00_7700 (sit down, 66 f) -> 7701 (loop, 61 f) -> 7702 (stand up, 56 f). Not in the ESD (the
  engine / event plays it); chosen by the pose [версия]: pelvis 0.10 m, head 0.60 m, right knee up 0.39 m, hands
  forward on the knee - the DS1 bonfire sit. Rejected: 7050/7051/7052 (ESD state machine 0 states 16-18) = lying
  on the back, head 0.10 m (coffin / "curl up"); 7711 cross-legged, 7721 both knees hugged (other variants).
  Death 6000: generic death (ESD state 71), pelvis to 0.29 m, no root motion [версия: which of 6000/6001/6010 the
  game picks depends on the hit direction - not decoded].
"""
from __future__ import annotations

import argparse
import json
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import cook_knight as K  # noqa: E402

CLIPS = [("drink_in", "a00_7585", False), ("drink", "a00_7586", False), ("drink_out", "a00_7587", False),
         ("drink_empty", "a00_7588", False), ("sit", "a00_7700", False), ("sit_loop", "a00_7701", True),
         ("stand", "a00_7702", False), ("death", "a00_6000", False)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out-dir", required=True)
    ap.add_argument("--merge-twist", action="store_true")
    a = ap.parse_args()
    t0 = time.time()
    out = Path(a.out_dir).expanduser()
    out.mkdir(parents=True, exist_ok=True)
    keep = [n for n in K.KEEP if not (a.merge_twist and n in K.TWIST)]
    sk = K.Skeleton(keep)
    hsk = K.load_hkx_skeleton()
    clips, stats = [], []
    for name, anim, loop in CLIPS:
        c = K.cook_clip(sk, hsk, anim)
        qc = K.quantize_clip(c)
        clips.append((name, K.anim_id(anim), loop, c, qc))
        stats.append(dict(name=name, src=c["src"], frames=c["frames"], loop=loop,
                          root_end=[round(float(x), 3) for x in c["root"][-1]]))
        print(f"clip {name:11s} {c['src']:24s} frames={c['frames']:3d} root_end={c['root'][-1].round(3).tolist()}",
              flush=True)
    st = dict(bones=len(sk.keep), clips=stats,
              ashanim=K.write_ashanim(out / "knight_prg.ashanim", len(sk.keep), clips),
              cook_seconds=round(time.time() - t0, 1))
    (out / "knight_prg.json").write_text(json.dumps(st, indent=1))
    print(json.dumps({k: v for k, v in st.items() if k != "clips"}))


if __name__ == "__main__":
    main()
