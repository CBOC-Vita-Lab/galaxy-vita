#!/usr/bin/env bash
# Host-side test of Performance mode's fast step down (GXV_AUTOQ_FAST60, Config::fast60, platform/vgx/src/vgx_autoq.cpp): a spike of pictures
# late for the GPU steps down within 0.5 s, never for CPU-late pictures, no oscillation, and Quality (target 30) and calm traces decide exactly
# as without it. Exit status 0 when every check passes.
#   tools/test_vgx_autoq_fast60.sh [output-dir]      (default: a temporary directory)
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
cxx=${CXX:-g++}
nice -n 10 "$cxx" -std=c++17 -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-omit-frame-pointer \
  "$root/tests/vgx_autoq_fast60.cpp" "$root/platform/vgx/src/vgx_autoq.cpp" -o "$out/vgx_autoq_fast60"
UBSAN_OPTIONS=halt_on_error=1 "$out/vgx_autoq_fast60" "${VERBOSE:+verbose}"
