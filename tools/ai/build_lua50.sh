#!/usr/bin/env bash
# Build stock Lua 5.0.3 for the host (x86_64 Linux) into ~/opt/lua50, patched so it loads DSR AI bytecode as is.
#
# DSR bytecode header: 1b 4c 75 61 50 01 04 08 04 06 08 09 09 08 = Lua 5.0, LE, int 4, size_t 8, Instruction 4,
# double. On LP64 Linux stock 5.0 has Instruction = unsigned long (8 B); DSR was built on LLP64 Windows (4 B).
# One-line patch: `typedef unsigned int Instruction;`. Nothing else changes.
#
# Some toolchains (conda gcc) stamp binaries "x86-64 ISA v3", which older CPUs refuse -> the note is stripped (harmless otherwise).
set -euo pipefail
PREFIX=${PREFIX:-$HOME/opt/lua50}
# Host compiler: the system gcc/cc by default; override with CC=/path/to/gcc (and AR/RANLIB/OBJCOPY if prefixed).
CC=${CC:-$(command -v gcc || command -v cc)}
AR=${AR:-ar}; RANLIB=${RANLIB:-ranlib}; OBJCOPY=${OBJCOPY:-objcopy}
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cd "$work"
curl -sSfLO https://www.lua.org/ftp/lua-5.0.3.tar.gz
echo "1193a61b0e08acaa6eee0eecf29709179ee49c71baebc59b682a25c3b5a45671  lua-5.0.3.tar.gz" | sha256sum -c -
tar xzf lua-5.0.3.tar.gz
cd lua-5.0.3
sed -i 's/^typedef unsigned long Instruction;/typedef unsigned int Instruction;/' src/llimits.h
grep -q 'typedef unsigned int Instruction;' src/llimits.h
nice -n 19 make CC=$CC AR="$AR rcu" RANLIB=$RANLIB MYCFLAGS="-O2 -std=gnu89 -w -fPIC" >/dev/null
for b in bin/lua bin/luac; do $OBJCOPY --remove-section .note.gnu.property $b 2>/dev/null || true; done
mkdir -p "$PREFIX"/{bin,lib,include,src}
cp bin/lua bin/luac "$PREFIX/bin/"
cp lib/liblua.a lib/liblualib.a "$PREFIX/lib/"
cp include/*.h "$PREFIX/include/"
cp -r src/. "$PREFIX/src/"   # internal headers (lopcodes.h ...) for tools that walk bytecode
echo "OK: $PREFIX"; "$PREFIX/bin/lua" -v
