#!/usr/bin/env bash
# mp30 (GXV_HRW_EVEN60, the Settings row "Allow in-between frame rates (50 fps)"): host test of port/hrw_mp.h, the multiplayer session's picture policy (start at an even 30, ramp to 60/30, plain 60 only where the
# governor says it holds, back down instead of skipping, the setting restores the old behaviour, single player unchanged), alone and closed-loop with the real governor (platform/vgx/src/vgx_hrw.h).
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${OUT:-/tmp/hrw_mp_test}; mkdir -p "$out"
${CXX:-g++} -std=c++17 -O1 -Wall -Wextra -I"$root/platform/vgx/src" -I"$root/platform/include" "$root/tests/hrw_mp_test.cpp" -o "$out/hrw_mp_test"
"$out/hrw_mp_test"
