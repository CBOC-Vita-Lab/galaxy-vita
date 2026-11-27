#!/usr/bin/env bash
# Host-side test of the GPU gate (Config::gpuGate, GXV_AUTOQ_GPUGATE; s1117b) in the automatic quality controller (platform/vgx/src/vgx_autoq.cpp): the thresholds,
# a CPU-limited scene is not lowered for the GPU's sake, a real overload gives the same trace, and the gate only removes step downs (a lockstep property).
# Exit status 0 when every check passes.
#   tools/test_vgx_autoq_gpugate.sh [output-dir]      (default: a temporary directory)     SAN= to build without the sanitizers
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
cxx=${CXX:-g++}
san=${SAN--fsanitize=address,undefined}
nice -n 10 "$cxx" -std=c++17 -O1 -g -Wall -Wextra $san -fno-omit-frame-pointer \
  "$root/tests/vgx_autoq_gpugate.cpp" "$root/platform/vgx/src/vgx_autoq.cpp" -o "$out/vgx_autoq_gpugate"
UBSAN_OPTIONS=halt_on_error=1 "$out/vgx_autoq_gpugate" "${VERBOSE:+verbose}"
