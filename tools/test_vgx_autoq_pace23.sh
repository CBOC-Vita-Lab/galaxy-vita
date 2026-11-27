#!/usr/bin/env bash
# Host-side test of GXV_PACE23 (Config::pace23) in the automatic quality controller (platform/vgx/src/vgx_autoq.cpp): the settle window after a load,
# the GPU blamed only when it held the pictures, direct and few size changes, the game thread's span in the census, Quality keeping 960x544.
# Exit status 0 when every check passes.
#   tools/test_vgx_autoq_pace23.sh [output-dir]      (default: a temporary directory)
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
cxx=${CXX:-g++}
san=${SAN--fsanitize=address,undefined}
nice -n 10 "$cxx" -std=c++17 -O1 -g -Wall -Wextra $san -fno-omit-frame-pointer \
  "$root/tests/vgx_autoq_pace23.cpp" "$root/platform/vgx/src/vgx_autoq.cpp" -o "$out/vgx_autoq_pace23"
UBSAN_OPTIONS=halt_on_error=1 "$out/vgx_autoq_pace23" "${VERBOSE:+verbose}"
