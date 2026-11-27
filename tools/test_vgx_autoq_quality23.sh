#!/usr/bin/env bash
# Host-side test of GXV_AUTOQ_QGUARD (Config::qg) in the automatic quality controller (platform/vgx/src/vgx_autoq.cpp "qguard"): quality given up
# only while it buys pictures. Closed loop with the steady 30 (port/frame_pace.h SteadyCadence) or the fixed 30 cap; replays of the console logs
# 483-c8P (pace60) and 491-c9SF1 (fast60). Exit status 0 when every check passes.
#   tools/test_vgx_autoq_quality23.sh [output-dir]      (default: a temporary directory; VERBOSE=1 prints every change)
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
cxx=${CXX:-g++}
san=${SAN--fsanitize=address,undefined}
nice -n 10 "$cxx" -std=c++17 -O1 -g -Wall -Wextra $san -fno-omit-frame-pointer \
  "$root/tests/vgx_autoq_quality23.cpp" "$root/platform/vgx/src/vgx_autoq.cpp" -o "$out/vgx_autoq_quality23"
UBSAN_OPTIONS=halt_on_error=1 "$out/vgx_autoq_quality23" ${VERBOSE:+verbose}
