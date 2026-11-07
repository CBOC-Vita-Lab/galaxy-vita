#!/usr/bin/env bash
# Host-side test of Performance mode, the 60 target of automatic quality (GXV_AUTOQ_TARGET=60, Config::targetFps 60, in
# platform/vgx/src/vgx_autoq.cpp): GPU-limited scenes step down until they fit 16.7 ms (cheaper effects and background first),
# CPU-limited scenes keep their size, room brings the picture back. Exit status 0 when every check passes.
#   tools/test_vgx_autoq_60.sh [output-dir]      (default: a temporary directory)
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
cxx=${CXX:-g++}
nice -n 10 "$cxx" -std=c++17 -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-omit-frame-pointer \
  "$root/tests/vgx_autoq_60.cpp" "$root/platform/vgx/src/vgx_autoq.cpp" -o "$out/vgx_autoq_60"
UBSAN_OPTIONS=halt_on_error=1 "$out/vgx_autoq_60" "${VERBOSE:+verbose}"
