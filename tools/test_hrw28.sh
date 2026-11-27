#!/usr/bin/env bash
# hrw28 (GXV_HRW significance set): host test of the set's pure logic (port/hrw_set.h: budget loop, selection, stagger, book) and the merge with several owners (vgx_hrw.h Compose / Uncompose). One file, nice 15.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${OUT:-/tmp/hrw28_test}; mkdir -p "$out"
${CXX:-g++} -std=c++17 -O1 -Wall -I"$root/platform/vgx/src" -I"$root/platform/include" "$root/tests/hrw28_test.cpp" -o "$out/hrw28_test"
"$out/hrw28_test"
