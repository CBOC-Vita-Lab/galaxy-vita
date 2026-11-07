#!/usr/bin/env bash
# Host-side test of Performance mode's way back on the steady 30 (GXV_AUTOQ_CADENCE_UP, Config::cadenceUp, platform/vgx/src/vgx_autoq.cpp):
# a scene on the steady 30 with 60 out of reach at any size climbs back (to 720x404 at most) and takes its steps off against 33.3 ms, a
# climb dearer than predicted goes back blocked, and nothing else decides differently; and the edge step's longer trial
# (GXV_AUTOQ_EDGECHECK, Config::edgeCheck). Exit status 0 when every check passes.
#   tools/test_vgx_autoq_cadence.sh [output-dir]      (default: a temporary directory)
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
cxx=${CXX:-g++}
nice -n 10 "$cxx" -std=c++17 -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-omit-frame-pointer \
  "$root/tests/vgx_autoq_cadence.cpp" "$root/platform/vgx/src/vgx_autoq.cpp" -o "$out/vgx_autoq_cadence"
UBSAN_OPTIONS=halt_on_error=1 "$out/vgx_autoq_cadence" "${VERBOSE:+verbose}"
