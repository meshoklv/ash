#!/bin/sh
# Build and run the src/items unit tests on the PC (host gcc from ~/opt/buildtools) on the real cooked data.
#   src/items/test/run.sh [path/to/items.ashit]      (default ~/ash-assets/cooked/items/items.ashit)
set -e
cd "$(dirname "$0")/../../.."
CC="${CC:-$HOME/opt/buildtools/bin/x86_64-conda-linux-gnu-gcc}"
OUT="${OUT:-build/items-native}"
mkdir -p "$OUT"
nice -n 19 "$CC" -std=c99 -O1 -g -Wall -Wextra -fsanitize=address,undefined -Isrc/items \
  -o "$OUT/test_items" src/items/test/test_items.c src/items/items.c -lm
# conda toolchain stamps "ISA needed: x86-64-v3"; the the build host Xeon is v2 (docs/build.md)
"$HOME/opt/buildtools/bin/x86_64-conda-linux-gnu-objcopy" --remove-section .note.gnu.property "$OUT/test_items" 2>/dev/null || \
  objcopy --remove-section .note.gnu.property "$OUT/test_items"
"$OUT/test_items" "$@"
