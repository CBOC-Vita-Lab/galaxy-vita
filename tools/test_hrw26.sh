#!/usr/bin/env bash
# hrw26 (GXV_HRW): host test of platform/vgx/src/vgx_hrw.h (camera maths vs a direct projection, the projection patch, byte-exact restore, conflicts, governor). One file, nice 15.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${OUT:-/tmp/hrw26_test}; mkdir -p "$out"
${CXX:-g++} -std=c++17 -O1 -Wall -I"$root/platform/vgx/src" -I"$root/platform/include" "$root/tests/hrw26_test.cpp" -o "$out/hrw26_test"
"$out/hrw26_test"
