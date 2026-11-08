#!/usr/bin/env bash
# Host test of GXV_FLOOR_SPAN (the floor census's split of "none of these") and GXV_STEADY30_GPUSCALE (Config::cadenceGpuScale):
# tests/vgx_floor_span.cpp drives platform/vgx/src/vgx_autoq.cpp with synthetic pictures. Exit status 0 when every check passes.
#   tools/test_vgx_floor_span.sh [output-dir]
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
cxx=${CXX:-g++}
nice -n 10 "$cxx" -std=c++17 -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-omit-frame-pointer \
  "$root/tests/vgx_floor_span.cpp" "$root/platform/vgx/src/vgx_autoq.cpp" -o "$out/vgx_floor_span"
UBSAN_OPTIONS=halt_on_error=1 "$out/vgx_floor_span"
