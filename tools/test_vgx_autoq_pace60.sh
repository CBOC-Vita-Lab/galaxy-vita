#!/usr/bin/env bash
# Host-side test of GXV_PACE60 (Config::p60) in the automatic quality controller (platform/vgx/src/vgx_autoq.cpp), closed loop with the steady 30
# (port/frame_pace.h SteadyCadence): the GPU model from the scene timers' split, the verdict "60 reachable" with hysteresis, Honeyhive / Good Egg /
# Beach Bowl / a borderline scene. Exit status 0 when every check passes.
#   tools/test_vgx_autoq_pace60.sh [output-dir]      (default: a temporary directory; VERBOSE=1 prints every change)
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
cxx=${CXX:-g++}
san=${SAN--fsanitize=address,undefined}
nice -n 10 "$cxx" -std=c++17 -O1 -g -Wall -Wextra $san -fno-omit-frame-pointer \
  "$root/tests/vgx_autoq_pace60.cpp" "$root/platform/vgx/src/vgx_autoq.cpp" -o "$out/vgx_autoq_pace60"
UBSAN_OPTIONS=halt_on_error=1 "$out/vgx_autoq_pace60" ${VERBOSE:+verbose}
