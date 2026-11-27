#!/usr/bin/env bash
# warp36 (GXV_HRW36_*: the ground-plane warp, the shaped player box, the parallax fallback): host test of the pure logic in port/hrw36.h. One file, no dependencies; CXX=g++ where clang has no sanitizer runtime.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${OUT:-/tmp/hrw36_test}; mkdir -p "$out"
${CXX:-g++} -std=c++17 -O1 -Wall -I"$root/platform/vgx/src" -I"$root/platform/include" "$root/tests/hrw36_test.cpp" -o "$out/hrw36_test"
"$out/hrw36_test"
