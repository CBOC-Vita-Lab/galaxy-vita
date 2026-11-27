#!/usr/bin/env bash
# Exact GXV_MASKFULL_TAIL (port/maskfull.h TailAlphaPredictor) host unit test.
# Exit status 0 when every check passes.   tools/test_vgx_maskfull_tail.sh [output-dir]
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
"${CXX:-clang++}" -std=c++17 -O1 -g -Wall -Wextra -Werror -fsanitize=undefined \
  -I"$root/platform/include" -I"$root/platform/vgx/src" \
  "$root/tests/vgx_maskfull_tail_test.cpp" -o "$out/vgx_maskfull_tail_test"
"$out/vgx_maskfull_tail_test"
