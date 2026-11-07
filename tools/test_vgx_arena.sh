#!/usr/bin/env bash
# The frame arenas' hold on the C heap (platform/vgx/src/vgx_arena.h): the growth rule, the floors, and a scripted tour of
# stages through four circulating frame buffers in a capped first-fit heap, against the old doubling (tests/vgx_arena.cpp).
#   tools/test_vgx_arena.sh      (host compiler; out-of-tree build in $OUT, default /tmp/vgx_arena)
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${OUT:-/tmp/vgx_arena}
mkdir -p "$out"
cd "$root"
${CXX:-clang++} -std=c++17 -O2 -Wall -Wextra -Iplatform/vgx/src tests/vgx_arena.cpp -o "$out/arena"
"$out/arena"
echo "vgx_arena: PASS"
