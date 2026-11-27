#!/usr/bin/env bash
# hrw27 (GXV_HRW stages d-h): host test of the partial draw's merge (vgx_hrw.h Compose / Uncompose), the sky plan, the verify-fresh probes, and the significance set (port/hrw_set.h). One file, nice 15.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${OUT:-/tmp/hrw27_test}; mkdir -p "$out"
${CXX:-g++} -std=c++17 -O1 -Wall -I"$root/platform/vgx/src" -I"$root/platform/include" "$root/tests/hrw27_test.cpp" -o "$out/hrw27_test"
"$out/hrw27_test"
