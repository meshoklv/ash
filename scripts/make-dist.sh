#!/usr/bin/env bash
# Build the memory-stick folder for a real PSP + a zip to send to the user .
#
#   scripts/make-dist.sh [--ref <git-ref>] [--worktree] [--no-check] [--no-game]
#
#   --ref REF     build sources from this commit (default HEAD) via `git archive` -> other agents' uncommitted
#                 work in the tree never ends up on the stick. Use the last commit where the viewer works.
#   --worktree    build from the working tree instead (quick local tests)
#   --no-check    skip the PPSSPPHeadless smoke run of every EBOOT
#   --no-game     skip the game EBOOT (ASHGAME)
#
# Output (dist/ is in .gitignore: it holds cooked FromSoftware data, never commit it):
#   dist/PSP/GAME/ASHBENCH/EBOOT.PBP    "Ash Bench"               full bench, MEMSIZE=0 (32 MB profile)
#   dist/PSP/GAME/ASHMEM64/EBOOT.PBP    "Ash Bench: memory 64MB"  free RAM in the 64 MB mode, MEMSIZE=1
#   dist/PSP/GAME/ASHVIEW/              "Ash: Asylum"             viewer + scene.ashm (hall with the knight)
#   dist/PSP/GAME/ASHGAME/              "Ash: Dark Souls"         the game: title -> character creation -> Asylum m18_01
#                                                                 -> crow -> Firelink Shrine m10_02, menus, save
#                                                                 (src/game/ds/ds_main.c + asy_*.c, docs/game.md)
#   dist/debug/                         ash_ds.map/.sym/.elf (+ bench) to decode crash.txt (tools/crash_decode.py);
#                                       in the zip too, NOT for the memory stick
#   every EBOOT dir gets ashexc.prx      src/diag kernel helper (CPU exception -> crash.txt), loaded via KUBridge
#   dist/ash-psp-<date>-<sha>.zip       the PSP/ tree, to unpack onto the root of the memory stick
#
# Cooked data comes from $ASH_COOKED (default ~/ash-assets/cooked).
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"; repo="$(cd "$here/.." && pwd)"
. "$here/env.sh"

ref=HEAD; worktree=0; check=1; game=1
while [ $# -gt 0 ]; do
  case "$1" in
    --ref) ref="$2"; shift 2 ;;
    --worktree) worktree=1; shift ;;
    --no-check) check=0; shift ;;
    --no-game) game=0; shift ;;
    *) sed -n 2,22p "$0"; exit 2 ;;
  esac
done
cooked="${ASH_COOKED:-$HOME/ash-assets/cooked}"

echo "== diskguard"; df -h "$repo" | tail -1; free -h | sed -n 2p
avail_kb=$(df -Pk "$repo" | awk 'NR==2{print $4}')
[ "$avail_kb" -gt $((3 * 1024 * 1024)) ] || { echo "STOP: <3 GB free on disk"; exit 1; }

# ---- sources
if [ $worktree = 1 ]; then
  src="$repo"; sha="$(git -C "$repo" rev-parse --short HEAD)-wt"
else
  sha="$(git -C "$repo" rev-parse --short "$ref")"
  src="$repo/build/dist-src/$sha"
  rm -rf "$src"; mkdir -p "$src"
  git -C "$repo" archive "$sha" | tar -x -C "$src"
  # the bench reads its git sha via `git rev-parse` in CMake; give the snapshot a stable value
  echo "$sha" > "$src/.ash_sha"
fi
bld="$repo/build/dist-build-$sha"
echo "== build $sha from $src"
psp-cmake -S "$src" -B "$bld" -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null
targets="ash_bench ash_mem64 ash_viewer ashexc"
nice -n 19 cmake --build "$bld" -j4 --target $targets
if [ $game = 1 ]; then
  if [ -f "$src/src/game/ds/ds_main.c" ] && nice -n 19 cmake --build "$bld" -j4 --target ash_ds; then :; else
    echo "WARN: ash_ds not built at $sha -> ASHGAME skipped"; game=0
  fi
fi

# ---- layout
dist="$repo/dist"; G="$dist/PSP/GAME"
rm -rf "$dist/PSP"; mkdir -p "$G"
mkdir -p "$G/ASHBENCH" "$G/ASHMEM64" "$G/ASHVIEW"
cp "$bld/src/bench/ASHBENCH/EBOOT.PBP" "$G/ASHBENCH/"
cp "$bld/src/bench/ASHMEM64/EBOOT.PBP" "$G/ASHMEM64/"
# src/diag: kernel helper for crash.txt + symbols for tools/crash_decode.py
dbg="$dist/debug"; rm -rf "$dbg"; mkdir -p "$dbg"
if [ -f "$bld/src/diag/ashexc.prx" ]; then
  cp "$bld/src/diag/ashexc.prx" "$G/ASHBENCH/"; cp "$bld/src/diag/ashexc.prx" "$G/ASHMEM64/"
else echo "WARN: no ashexc.prx at $sha (no crash.txt on the PSP)"; fi
for e in "$bld/src/bench/ash_bench"; do
  [ -f "$e.elf" ] && { cp "$e.map" "$dbg/"; "$PSPDEV/bin/psp-nm" -n --defined-only "$e.elf" > "$dbg/$(basename "$e").sym"; }
done

# repack an ELF with our own PARAM.SFO title + icon (does not touch the other agents' CMakeLists)
repack() { # <elf> <out_dir> <title> <icon>
  local tmp; tmp="$(mktemp -d)"
  "$PSPDEV/bin/mksfoex" -d MEMSIZE=1 -s APP_VER=01.00 "$3" "$tmp/PARAM.SFO"
  "$PSPDEV/bin/pack-pbp" "$2/EBOOT.PBP" "$tmp/PARAM.SFO" "$4" NULL NULL NULL NULL "$1" NULL >/dev/null
  rm -rf "$tmp"
}
repack "$bld/src/viewer/ash_viewer" "$G/ASHVIEW" "Ash: Asylum" "$repo/dist-res/icon_asylum.png"
[ -f "$cooked/hall.ashm" ] && cp "$cooked/hall.ashm" "$G/ASHVIEW/scene.ashm" || echo "WARN: no $cooked/hall.ashm (viewer will say 'scene not found')"
if [ $game = 1 ]; then
  mkdir -p "$G/ASHGAME/ai/aiCommon" "$G/ASHGAME/ai/m18_01_00_00"
  repack "$bld/src/game/ds/ash_ds" "$G/ASHGAME" "Ash: Dark Souls" "$repo/dist-res/icon_asylum.png"
  [ -f "$bld/src/diag/ashexc.prx" ] && cp "$bld/src/diag/ashexc.prx" "$G/ASHGAME/"
  e="$bld/src/game/ds/ash_ds"
  if [ -f "$e.elf" ]; then
    cp "$e.map" "$e.elf" "$dbg/"; "$PSPDEV/bin/psp-nm" -n --defined-only "$e.elf" > "$dbg/ash_ds.sym"
  fi
  for f in world/zone_m18_01.pak world/zone_m18_01.ashmip m18_01.ashevt combat.ashcb knight.ashc knight.ashanim \
           knight_combat.ashanim knight_prg.ashanim hollow.ashc hollow.ashanim hollow_combat.ashanim boss.ashc \
           boss.ashanim boss.ashboss boss_name.ashtx ui.ashui items/items.ashit items/items.ashui \
           firelink/zone_m10_02.pak firelink/zone_m10_02.ashmip firelink/crestfallen.ashc firelink/crestfallen.ashanim \
           firelink/firelink.txt menu/menu.ashui menu/menu.ashcls menu/menu_idle.ashanim; do
    [ -f "$cooked/$f" ] && cp "$cooked/$f" "$G/ASHGAME/" || echo "WARN: no $cooked/$f for ASHGAME"
  done
  cp "$cooked"/menu/cls_*.ashc "$cooked"/menu/head_*.ashc "$G/ASHGAME/"   # class models (player body + creation preview)
  # DSR AI bytecode (aiCommon + the demon 223200 + the hollow 250000), repacked 32-bit / float for the PSP
  ai="${ASH_AI:-$HOME/ash-assets/ai}"; py="$repo/tools/.venv/bin/python"
  if [ -f "$ai/aiCommon/load_order.txt" ]; then
    for f in "$ai"/aiCommon/*.luac; do
      "$py" "$repo/tools/ai/luac_repack.py" "$f" "$G/ASHGAME/ai/aiCommon/$(basename "$f")" --number float --strip >/dev/null
    done
    for b in 223200 250000; do
      "$py" "$repo/tools/ai/luac_repack.py" "$ai/m18_01_00_00/${b}_battle.luac" "$G/ASHGAME/ai/m18_01_00_00/${b}_battle.luac" \
        --number float --strip >/dev/null
    done
    cp "$ai/aiCommon/load_order.txt" "$G/ASHGAME/ai/aiCommon/"
    cp "$ai/think_223200.txt" "$ai/think_250022.txt" "$G/ASHGAME/ai/"
  else
    echo "WARN: no $ai (DSR AI) -> hollows idle, the demon on its C state machine"
  fi
fi

cat > "$dist/README.txt" <<EOF
Ash for PSP, build $sha ($(date +%Y-%m-%d)).
Copy the PSP folder onto the ROOT of the memory stick (merge with the existing PSP folder).
XMB -> Game -> Memory Stick:
  Ash Bench               - hardware test, ~2-3 min, then press START. Result: PSP/GAME/ASHBENCH/results.txt
  Ash Bench: memory 64MB  - a few seconds. Result: PSP/GAME/ASHMEM64/results.txt
  Ash: Asylum             - Asylum Demon hall viewer (L/R = camera, START = exit)
  Ash: Dark Souls         - THE GAME: title -> new game (10 classes) -> Northern Undead Asylum -> Asylum Demon ->
                            crow -> Firelink Shrine. Stick move, X roll/run(hold), [] attack, /\ heavy, R shield,
                            R+/\ parry, L lock-on, O Estus, d-pad up = action (doors, items, bonfire, talk),
                            START = menu (inventory, equipment, status, settings, back to title).
                            Saves itself at bonfires and zone changes: XMB Saved Data "Ash: DS1" (delete it there).
Needs custom firmware (ARK-4 / PRO / LME). See docs/hw-pitfalls.md
After a freeze/crash: copy PSP/GAME/<folder>/log.txt, log_prev.txt, crash.txt, crash_prev.txt off the stick
BEFORE starting the game again (see docs/hw-pitfalls.md). debug/ in the zip = symbols, not for the stick.
EOF

# ---- smoke check in the emulator (on a copy: results.txt must not land in dist)
if [ $check = 1 ]; then
  echo "== smoke check (PPSSPPHeadless; emulator numbers are not hardware numbers)"
  st="$repo/build/dist-check"; rm -rf "$st"; mkdir -p "$st"; cp -r "$dist/PSP" "$st/"
  shim="$repo/build/ppsspp-shm-shim.so"; [ -f "$shim" ] && export LD_PRELOAD="$shim"
  run() { # <dir> <emulated_s>
    "$here/emu-slot.sh" timeout 900 "$PPSSPP_HEADLESS" "$st/PSP/GAME/$1/EBOOT.PBP" --memstick="$st" --graphics=software \
      --timeout-emulated="$2" --timeout-wall=800 --screenshot-save="$st/$1.png" > "$st/$1.log" 2>&1 || true
    if [ -s "$st/$1.png" ]; then echo "  $1: screenshot $st/$1.png"; else echo "  $1: NO SCREENSHOT"; fi
  }
  run ASHBENCH 200; run ASHMEM64 10; run ASHVIEW 6
  [ -d "$G/ASHGAME" ] && run ASHGAME 12
  unset LD_PRELOAD
  for d in ASHBENCH ASHMEM64; do
    if [ -s "$st/PSP/GAME/$d/results.txt" ]; then echo "  $d: $(grep -m1 '^GATE' "$st/PSP/GAME/$d/results.txt")"
    else echo "  $d: NO results.txt"; fi
  done
  grep -hE 'ash-viewer: (scene|loaded|error)|not found|out of memory' "$st/ASHVIEW.log" | head -3 || true
fi

# ---- zip (python: no zip binary in the container)
zip="$dist/ash-psp-$(date +%Y%m%d)-$sha.zip"
rm -f "$dist"/ash-psp-*.zip
python3 -I - "$dist" "$zip" <<'PY'
import sys, os, zipfile
root, out = sys.argv[1], sys.argv[2]
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
    z.write(os.path.join(root, "README.txt"), "README.txt")
    for d, _, files in os.walk(os.path.join(root, "debug")):
        for f in sorted(files):
            z.write(os.path.join(d, f), os.path.relpath(os.path.join(d, f), root))
    for d, _, files in os.walk(os.path.join(root, "PSP")):
        for f in sorted(files):
            p = os.path.join(d, f)
            z.write(p, os.path.relpath(p, root))
print("zip:", out, os.path.getsize(out), "B")
PY
( cd "$dist" && find PSP -type f -printf '%10s  %p\n' | sort -k2 )
