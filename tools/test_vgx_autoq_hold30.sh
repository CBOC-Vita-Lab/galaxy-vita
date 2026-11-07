#!/usr/bin/env bash
# Host-side test of Quality mode judged by the pictures (GXV_AUTOQ_HOLD30, Config::hold30 in platform/vgx/src/vgx_autoq.cpp): under the
# fixed 30 cap a size gives way only for pictures late for the GPU over two windows in a row, never for the GPU's busy share alone or for
# the game's CPU; a calm stretch climbs back on trial, a failed trial backs off. Exit status 0 when every check passes.
#   tools/test_vgx_autoq_hold30.sh [output-dir] [verbose]      (default: a temporary directory)
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
cxx=${CXX:-g++}
nice -n 10 "$cxx" -std=c++17 -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-omit-frame-pointer \
  "$root/tests/vgx_autoq_hold30.cpp" "$root/platform/vgx/src/vgx_autoq.cpp" -o "$out/vgx_autoq_hold30"
UBSAN_OPTIONS=halt_on_error=1 "$out/vgx_autoq_hold30" "${2:-}"
