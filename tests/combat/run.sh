#!/bin/sh
# Build and run the combat unit tests on the PC (host gcc from ~/opt/buildtools), on the real cooked data.
#   tests/combat/run.sh [path/to/combat.ashcb]
set -e
cd "$(dirname "$0")/../.."
CC="${CC:-$HOME/opt/buildtools/bin/x86_64-conda-linux-gnu-gcc}"
OUT="${OUT:-tests/out/combat}"
mkdir -p "$OUT"
nice -n 19 "$CC" -std=c99 -O1 -g -Wall -Wextra -Wno-unused-parameter -fsanitize=address,undefined \
  -o "$OUT/test_combat" tests/combat/test_combat.c src/combat/combat.c src/combat/cbdata.c -lm
# conda toolchain stamps "ISA needed: x86-64-v3"; the the build host Xeon is v2 (docs/build.md)
"$HOME/opt/buildtools/bin/x86_64-conda-linux-gnu-objcopy" --remove-section .note.gnu.property "$OUT/test_combat" 2>/dev/null || \
  objcopy --remove-section .note.gnu.property "$OUT/test_combat"
"$OUT/test_combat" "$@"
