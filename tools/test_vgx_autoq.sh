#!/usr/bin/env bash
# Host-side test of the automatic quality controller (dynamic resolution): tests/vgx_autoq_test.cpp drives
# platform/vgx/src/vgx_autoq.cpp with synthetic frame timings (Observatory-like CPU-limited, plaza-like GPU-limited,
# spikes, loads, under-predicted costs, noise). Exit status 0 when every check passes.
#   tools/test_vgx_autoq.sh [output-dir]      (default: a temporary directory)
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
cxx=${CXX:-g++}
nice -n 10 "$cxx" -std=c++17 -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-omit-frame-pointer \
  "$root/tests/vgx_autoq_test.cpp" "$root/platform/vgx/src/vgx_autoq.cpp" -o "$out/vgx_autoq_test"
UBSAN_OPTIONS=halt_on_error=1 "$out/vgx_autoq_test"
