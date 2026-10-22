#!/usr/bin/env bash
# Native walkthrough test of the event interpreter on the cooked m18_01 script (no emulator needed):
#   scripts/test-events.sh [file.ashevt] [-v]
# Cooks ~/ash-assets/cooked/m18_01.ashevt if missing, builds src/event + src/event/test/evtest.c with the host gcc
# (~/opt/buildtools, docs/build.md) into build/evtest-native, runs it. Exit code = test result.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"; repo="$(cd "$here/.." && pwd)"
evt="${1:-$HOME/ash-assets/cooked/m18_01.ashevt}"; shift || true
if [ ! -f "$evt" ]; then
  nice -n 19 "$repo/tools/.venv/bin/python" "$repo/tools/cook_events.py" --out "$evt"
fi
CC="$HOME/opt/buildtools/bin/x86_64-conda-linux-gnu-gcc"
mkdir -p "$repo/build"
nice -n 19 "$CC" -std=c99 -O1 -g -Wall -Wextra -Wno-unused-parameter -fsanitize=address,undefined \
  -o "$repo/build/evtest-native" "$repo/src/event/event.c" "$repo/src/event/test/evtest.c" -lm
# conda gcc marks binaries x86-64 v3; the build host Xeon is v2 (docs/build.md)
"$HOME/opt/buildtools/bin/x86_64-conda-linux-gnu-objcopy" --remove-section .note.gnu.property "$repo/build/evtest-native" 2>/dev/null || true
exec "$repo/build/evtest-native" "$evt" "$@"
