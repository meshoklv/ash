#!/usr/bin/env python3
"""Firelink Shrine texts for the full game (src/game/ds/ds_main.c): area name, "talk" prompt, the Crestfallen
Warrior's opening lines - from the DSR copy's FMG (msg/RUSSIAN), never typed into git. Output (outside git):
    ~/ash-assets/cooked/firelink/firelink.txt   key=value lines, UTF-8, "\\n" = line break
Sources [проверено: tools/cook_fltext.py --list prints the FMG ids]:
    area     item.msgbnd FMG 19 id 1020   PlaceName "Храм Огня"
    talk     menu.msgbnd FMG 30 id 10010200   event text "Поговорить"
    loading  menu.msgbnd FMG 92 id 47102   first line ("Загрузка...")
    say<N>   menu.msgbnd FMG 1 ids 37000000.. (Conversation; the block of t102627 = the Crestfallen Warrior
             [версия по содержанию, docs/firelink.md §4])
    closed   own text "Путь закрыт" [придумал] (DS1 has no such message: the exits are cut for the slice)
  tools/.venv/bin/python tools/cook_fltext.py [--out PATH] [--list]
"""
import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import cook_ui as cu  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", type=Path, default=Path.home() / "ash-assets/cooked/firelink/firelink.txt")
    ap.add_argument("--list", action="store_true")
    a = ap.parse_args()
    f = cu.load_fmgs("RUSSIAN")
    get = lambda b, i, k: f.get((b, i), {}).get(k)
    out = {
        "area": get("item", 19, 1020),
        "talk": get("menu", 30, 10010200),
        "loading": (get("menu", 92, 47102) or "").split("\n")[0],
        "closed": "Путь закрыт",
    }
    n = 0
    for k in range(37000000, 37000010):
        t = get("menu", 1, k)
        if t is None:
            break
        out[f"say{n}"] = t
        n += 1
    for k, v in out.items():
        if not v:
            raise SystemExit(f"FMG text missing: {k}")
    a.out.parent.mkdir(parents=True, exist_ok=True)
    with open(a.out, "w", encoding="utf-8") as fp:
        for k, v in out.items():
            fp.write(f"{k}=" + v.replace("\r", "").replace("\n", "\\n") + "\n")
    if a.list:
        for k, v in out.items():
            print(k, repr(v))
    print(f"firelink texts: {len(out)} keys ({n} lines of the Crestfallen Warrior) -> {a.out}")


if __name__ == "__main__":
    main()
