#!/usr/bin/env bash
# GXV_SLIP through the production perf.cpp on a virtual clock (tests/pace_slip_perf.cpp; a model, not a console measurement).
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
"${CXX:-clang++}" -std=c++20 -O1 -g -D__vita__ -DTARGET_PC=1 -DNDEBUG=1 -fms-extensions -I"$root/platform/include" -I"$root/vita/runtime" \
  -I"$root/decomp/libs/RVL_SDK/include" -idirafter "${VITASDK:-/opt/vitasdk}/arm-vita-eabi/include" -include "$root/platform/include/port/compat.h" \
  -Wno-register -Wno-unknown-pragmas -Wno-deprecated-volatile -Wno-deprecated-declarations "$root/tests/pace_slip_perf.cpp" -o "$out/pace_slip_perf"
cd "$root/tests"
echo "-- off (default)";               "$out/pace_slip_perf" 14 6 off
echo "-- GXV_SLIP=1 (cap 15), 34 ms";   GXV_SLIP=1 "$out/pace_slip_perf" 14 6 slip
echo "-- GXV_SLIP=1 (cap 15), 38 ms";   GXV_SLIP=1 "$out/pace_slip_perf" 16 6 slip
echo "-- GXV_SLIP=1 (cap 15), 40 ms: over the cap";  GXV_SLIP=1 "$out/pace_slip_perf" 17 6 skip
echo "-- GXV_SLIP=1 GXV_SLIP_MAX=10, 38 ms: over the cap"; GXV_SLIP=1 GXV_SLIP_MAX=10 "$out/pace_slip_perf" 16 6 skip
echo "-- GXV_SLIP=1 GXV_SLIP_MAX=3, 34 ms: 1.9 % is inside"; GXV_SLIP=1 GXV_SLIP_MAX=3 "$out/pace_slip_perf" 14 6 slip
