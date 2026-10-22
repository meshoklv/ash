#!/usr/bin/env bash
# Native scenario run of the Asylum game logic (no emulator): builds build/asy-native, stages the cooked data in
# build/stage-native (DSR AI bytecode as is: native Lua 5.0.3 is double/64-bit), runs one scenario.
#   tests/native/run.sh <route|hollow|death|boss> [max_seconds] [-v]
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"; repo="$(cd "$here/../.." && pwd)"
sc="${1:?scenario}"; shift
cooked="${ASH_COOKED:-$HOME/ash-assets/cooked}"; ai="$HOME/ash-assets/ai"; LUA="${LUA50:-$HOME/opt/lua50}"
CC="$HOME/opt/buildtools/bin/x86_64-conda-linux-gnu-gcc"
PSPSDK_INC="$HOME/opt/pspdev/psp/sdk/include"
out="$repo/build/asy-native${ASH_UBSAN:+-ubsan}"
# ASH_UBSAN=1: UBSan alignment build (docs/hw-pitfalls.md): a misaligned 16/32-bit or float access traps on the PSP
# (AdEL/AdES) but passes silently in PPSSPP and on x86; reports go to stderr, the run goes on.
san=""; [ -n "${ASH_UBSAN:-}" ] && san="-fsanitize=alignment -fsanitize-recover=alignment -static-libubsan"
srcs=("$repo/src/game/asy_game.c" "$repo/src/game/asy_auto.c" "$repo/src/game/anim.c" "$repo/src/game/chr.c"
      "$repo/src/game/camera.c" "$repo/src/game/input.c" "$repo/src/game/pcol.c" "$repo/src/world/world.c"
      "$repo/src/event/event.c" "$repo/src/combat/combat.c" "$repo/src/combat/cbdata.c" "$repo/src/boss/boss.c"
      "$repo/src/boss/boss_ai.c" "$repo/src/ai/aihost.c" "$repo/src/progress/progress.c" "$repo/src/progress/save.c"
      "$repo/src/progress/prg_hud.c" "$repo/src/ui/ui.c" "$repo/src/ui/hud.c" "$repo/src/items/items.c" "$here/asy_native.c" "$here/psp_stubs.c")
newest=$(ls -t "${srcs[@]}" "$repo"/src/*/*.h 2>/dev/null | head -1)
if [ ! -x "$out" ] || [ "$newest" -nt "$out" ]; then
  nice -n 19 "$CC" -std=gnu99 -O1 -g -w $san -I"$PSPSDK_INC" -I"$LUA/include" \
    -I"$repo/src/game" -I"$repo/src/world" -I"$repo/src/event" -I"$repo/src/combat" -I"$repo/src/boss" \
    -I"$repo/src/ai" -I"$repo/src/progress" -I"$repo/src/ui" -I"$repo/src/items" \
    "${srcs[@]}" "$LUA/lib/liblua.a" "$LUA/lib/liblualib.a" -lm -o "$out"
  "$HOME/opt/buildtools/bin/x86_64-conda-linux-gnu-objcopy" --remove-section .note.gnu.property "$out" 2>/dev/null || true
fi
stage="$repo/build/stage-native"
mkdir -p "$stage/ai/aiCommon" "$stage/ai/m18_01_00_00"
for f in world/zone_m18_01.pak m18_01.ashevt combat.ashcb knight.ashc knight.ashanim knight_combat.ashanim \
         knight_prg.ashanim hollow.ashc hollow.ashanim boss.ashc boss.ashanim boss.ashboss boss_name.ashtx ui.ashui; do
  b="$(basename "$f")"; [ "$cooked/$f" -nt "$stage/$b" ] && ln -sf "$cooked/$f" "$stage/$b"
done
if [ ! -f "$stage/ai/aiCommon/load_order.txt" ]; then
  ln -sf "$ai"/aiCommon/*.luac "$stage/ai/aiCommon/"; cp "$ai/aiCommon/load_order.txt" "$stage/ai/aiCommon/"
  ln -sf "$ai/m18_01_00_00/223200_battle.luac" "$ai/m18_01_00_00/250000_battle.luac" "$stage/ai/m18_01_00_00/"
  cp "$ai/think_223200.txt" "$ai/think_250022.txt" "$stage/ai/"
fi
[ "$sc" = boss ] || rm -f "$stage/ash.sav"
exec nice -n 19 "$out" "$stage" "$sc" "$@"
