#!/usr/bin/env bash
# hrw41 (GXV_HRW41_DEPTH / GXV_HRW41_BOXKEY / GXV_HRW_STAGE_AUTO_RETRY): host tests of the pure logic. No dependencies; CXX=g++ where clang has no sanitizer runtime.
#   tests/hrw41_test.cpp           port/hrw41.h and the ray-cast model of the pair's second picture (prints the model's error table)
#   tests/pace33_patch9_test.cpp   port/hrw_stage.h: the automatic fallback and its way back
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${OUT:-/tmp/hrw41_test}; mkdir -p "$out"
${CXX:-g++} -std=c++17 -O2 -Wall -I"$root/platform/include" "$root/tests/hrw41_test.cpp" -o "$out/hrw41_test"
"$out/hrw41_test"
${CXX:-g++} -std=c++20 -O1 -Wall -I"$root/platform/include" "$root/tests/pace33_patch9_test.cpp" -o "$out/pace33_patch9_test"
"$out/pace33_patch9_test"
