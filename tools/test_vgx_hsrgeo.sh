#!/usr/bin/env bash
# GXV_HSR_ORDER_TIES=4 (ties25): the geometry proofs (tests/vgx_hsrgeo_test.cpp: triangle distances, the pair job against brute force, soundness of "free"
# against a reference rasteriser, negative controls) and the planner's model test with the oracle (tests/vgx_hsrorder_plan.cpp, ties=4 part). No GL.
#   tools/test_vgx_hsrgeo.sh [output-dir]       (HSRGEO_SCENES=<n>: more camera scenes in check C, default 5000)
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
export CPLUS_INCLUDE_PATH="$root/platform/include${CPLUS_INCLUDE_PATH:+:$CPLUS_INCLUDE_PATH}"
nice -n 10 "${CXX:-g++}" -std=c++20 -O2 -Wall -Wextra -I"$root/platform/vgx/src" "$root/tests/vgx_hsrgeo_test.cpp" -o "$out/geo"
nice -n 10 "${CXX:-g++}" -std=c++20 -O1 -Wall -I"$root/platform/vgx/src" "$root/tests/vgx_hsrorder_plan.cpp" -o "$out/plan"
if "${CXX:-g++}" -std=c++20 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -I"$root/platform/vgx/src" "$root/tests/vgx_hsrgeo_test.cpp" -o "$out/geo_asan" 2>/dev/null; then
  ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 "$out/geo_asan" > "$out/geo_asan.log" 2>&1 && echo "ASan / UBSan run of the geometry test: clean" || { echo "FAILED: sanitizer run"; tail -5 "$out/geo_asan.log"; exit 1; }
fi
"$out/geo" | tee "$out/geo.log" | tail -30
grep -q FAIL "$out/geo.log" && { echo "FAILED: geometry test"; exit 1; }
"$out/plan" | tee "$out/plan.log" | grep -E "ties=4|ties=6|depth functions|NEGATIVE CONTROL: (an oracle|moving)|^PASS: hsrorder|FAIL"
grep -q FAIL "$out/plan.log" && { echo "FAILED: planner model test"; exit 1; }
echo "PASS: geometry proofs and the ties=4 planner (see $out/geo.log, plan.log)"
