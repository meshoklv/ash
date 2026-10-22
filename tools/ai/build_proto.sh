#!/usr/bin/env bash
# Build the PC AI prototype (src/ai + src/ai/aitest.c) against host Lua 5.0.3 from tools/ai/build_lua50.sh.
set -euo pipefail
cd "$(dirname "$0")/../.."
export PATH=$HOME/opt/buildtools/bin:$PATH
LUA=${LUA50:-$HOME/opt/lua50}
mkdir -p build/host
nice -n 19 x86_64-conda-linux-gnu-gcc -O2 -g -std=gnu99 -Wall -Wextra -Wno-unused-parameter \
  -I"$LUA/include" -Isrc/ai src/ai/aihost.c src/ai/aitest.c "$LUA/lib/liblua.a" "$LUA/lib/liblualib.a" -lm \
  -o build/host/ai_proto
x86_64-conda-linux-gnu-objcopy --remove-section .note.gnu.property build/host/ai_proto
echo build/host/ai_proto
