#!/usr/bin/env bash
# Native unit test of src/progress (progress + save container), host gcc from scripts/env.sh. Prints PASS n/n.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"; repo="$(cd "$here/.." && pwd)"
. "$here/env.sh"
out="$repo/build/native"; mkdir -p "$out"
nice -n 19 x86_64-conda-linux-gnu-gcc -std=c99 -O1 -g -Wall -Wextra -Wno-unused-parameter -I "$repo/src/ui" \
  -o "$out/prgtest" "$repo/src/progress/test/prgtest.c" "$repo/src/progress/progress.c" "$repo/src/progress/save.c" -lm
x86_64-conda-linux-gnu-objcopy --remove-section .note.gnu.property "$out/prgtest" 2>/dev/null || objcopy --remove-section .note.gnu.property "$out/prgtest"   # ISA note (docs/build.md)
cd "$out" && ./prgtest
