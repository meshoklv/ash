#!/usr/bin/env bash
# Native unit test of src/save (format, damage detection, file store, progress bridge). Prints PASS n/n.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"; repo="$(cd "$here/.." && pwd)"
. "$here/env.sh"
out="$repo/build/native/save"; mkdir -p "$out"
nice -n 19 x86_64-conda-linux-gnu-gcc -std=c99 -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers \
  -I "$repo/src/save" -I "$repo/src/progress" -I "$repo/src/ui" -o "$out/savetest" \
  "$repo/src/save/test/native.c" "$repo/src/save/savegame.c" "$repo/src/save/savestore.c" \
  "$repo/src/progress/progress.c" "$repo/src/progress/save.c" -lm
x86_64-conda-linux-gnu-objcopy --remove-section .note.gnu.property "$out/savetest" 2>/dev/null || objcopy --remove-section .note.gnu.property "$out/savetest"   # ISA note (docs/build.md)
cd "$out" && ./savetest
