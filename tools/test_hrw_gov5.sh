#!/usr/bin/env bash
# gov5 (GXV_HRW_GOV5): host test of the 60 Assist governor in mode 5 (platform/vgx/src/vgx_hrw.h, Params::warp): never engages where plain 60 holds, the GPU ladder first, the pair where it cannot, leaves when plain 60 holds, no flapping.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${OUT:-/tmp/hrw_gov5_test}; mkdir -p "$out"
${CXX:-g++} -std=c++17 -O1 -Wall -I"$root/platform/vgx/src" -I"$root/platform/include" "$root/tests/hrw_gov5_test.cpp" -o "$out/hrw_gov5_test"
"$out/hrw_gov5_test"
