#!/usr/bin/env python3
"""Export one enemy's NpcParam + NpcThinkParam rows as the key=value text the AI prototype reads.

    tools/.venv/bin/python tools/ai/think_export.py --npc 250022 --think 250000 > ~/ash-assets/ai/think_250022.txt

Keys `excel.<n>` are the values ai:GetExcelParam(n) returns, n = AI_EXCEL_THINK_PARAM_TYPE__* from ai_define.lua
(value table in the ai-facts design notes). The field mapping (Paramdex DS1R names on the right) is ours [версия]:
the Lua side only ever reads n = 1, 2, 3, 4, 8, 11, 19, 23 for the hollow.
"""
from __future__ import annotations

import argparse
import dataclasses
import logging

logging.disable(logging.WARNING)
from soulstruct.darksouls1r.params import GameParamBND  # noqa: E402
import os  # noqa: E402
from pathlib import Path  # noqa: E402

DSR = Path(os.environ.get("ASH_DSR", Path.home() / "ash-assets/dsr"))

# AI_EXCEL_THINK_PARAM_TYPE__* value -> NpcThinkParam field (soulstruct names)
EXCEL = {
    1: "MaxRetreatDistance",          # maxBackhomeDist
    2: "BattleRetreatDistance",       # backhomeDist
    3: "RetreatBattleStartDistance",  # backhomeBattleDist
    4: "NonBattleActLife",            # nonBattleActLife
    5: "BattleStartDistance",         # BattleStartDist
    7: "StuckAnimationID",            # CannotMoveAction (ezState used when stuck: top_goal.lua)
    8: "BattleID",                    # battleGoalID
    9: "SearchTimeBeforeRetreat",     # BackHome_LookTargetTime
    10: "SearchDistanceBeforeRetreat",  # BackHome_LookTargetDist
    11: "RetreatTimeAfterHittingEnemyWall",  # BackHomeLife_OnHitEnemyWall
    19: "HelpCallForgetTime",         # callHelp_ForgetTimeByArrival
    23: "AdmirerAttribute",           # thinkAttr_doAdmirer
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--npc", type=int, required=True)
    ap.add_argument("--think", type=int, required=True)
    a = ap.parse_args()
    gp = GameParamBND.from_path(DSR / "param/GameParam/GameParam.parambnd.dcx")

    def P(n):
        return next(v for k, v in gp.params.items() if k.endswith("\\" + n))

    th = P("NpcThinkParam")[a.think]
    npc = P("NpcParam")[a.npc]
    tf = {f.name: getattr(th, f.name) for f in dataclasses.fields(th)}
    nf = {f.name: getattr(npc, f.name) for f in dataclasses.fields(npc)}
    print(f"# NpcParam {a.npc} ({nf['Name']}) NpcThinkParam {a.think} ({tf['Name']})")
    for k in ("NearDistance", "MidDistance", "FarDistance", "OutOfRangeDistance", "SightDistance",
              "HearingDistance", "BattleStartDistance", "LogicID", "BattleID"):
        print(f"think.{k}={float(tf[k])}")
    for k in ("MaximumHP", "TurnVelocity", "HitRadius", "BehaviorVariationID", "MoveAnimationID"):
        print(f"npc.{k}={float(nf[k])}")
    for n, field in EXCEL.items():
        v = tf.get(field)
        if v is None:
            print(f"# excel.{n}: field {field} not in this soulstruct version")
            continue
        print(f"excel.{n}={float(v)}")


if __name__ == "__main__":
    main()
