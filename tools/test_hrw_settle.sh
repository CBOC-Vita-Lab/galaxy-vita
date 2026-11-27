#!/usr/bin/env bash
# stutter37 (GXV_HRW_SETTLE_RESUME: pairs again sooner after a pause or a closed fade in a scene that had settled): host test of port/hrw_settle.h. One file, no dependencies.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${OUT:-/tmp/hrw_settle_test}; mkdir -p "$out"
${CXX:-g++} -std=c++17 -O1 -Wall -Wextra -I"$root/platform/include" "$root/tests/hrw_settle_test.cpp" -o "$out/hrw_settle_test"
"$out/hrw_settle_test"
