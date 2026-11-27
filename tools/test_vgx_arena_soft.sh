#!/usr/bin/env bash
# GXV_ARENA_SOFT: the vertex arena's soft growth (platform/vgx/src/vgx_arena.h), on the host (tests/vgx_arena_soft.cpp).
#   tools/test_vgx_arena_soft.sh      (host compiler; out-of-tree build in $OUT, default /tmp/vgx_arena_soft)
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${OUT:-/tmp/vgx_arena_soft}
mkdir -p "$out"
cd "$root"
${CXX:-clang++} -std=c++17 -O1 -Wall -Wextra -Iplatform/vgx/src tests/vgx_arena_soft.cpp -o "$out/arena_soft"
"$out/arena_soft"
# the old arena test must still pass with the new allocator hook in vgx_cmd.h
OUT="$out/old" "$root/tools/test_vgx_arena.sh"
