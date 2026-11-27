#!/usr/bin/env bash
# hrwfeel36: host test of port/hrw_feel.h (the "hrw: feel |" line's counting). No dependencies; CXX=g++ where clang has no sanitizer runtime.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${OUT:-/tmp/hrw_feel_test}; mkdir -p "$out"
${CXX:-g++} -std=c++17 -O2 -Wall -I"$root/platform/include" "$root/tests/hrw_feel_test.cpp" -o "$out/hrw_feel_test"
"$out/hrw_feel_test"
