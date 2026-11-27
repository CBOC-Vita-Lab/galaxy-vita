#!/usr/bin/env bash
# Host test of GXV_DIRECT_DISPLAY (direct24): the decision, the buffer rotation and the census against a fake world (tests/vgx_directdisplay_test.cpp),
# plain and under ASan + UBSan. No GL, no SDK.   tools/test_vgx_directdisplay.sh [output-dir]
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
nice -n 10 "${CXX:-g++}" -std=gnu++20 -O1 -g -Wall -Wextra -I"$root/platform/vgx/src" "$root/tests/vgx_directdisplay_test.cpp" -o "$out/vgx_directdisplay_test"
"$out/vgx_directdisplay_test"
nice -n 10 "${CXX:-g++}" -std=gnu++20 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined -Wall -Wextra -I"$root/platform/vgx/src" "$root/tests/vgx_directdisplay_test.cpp" -o "$out/vgx_directdisplay_test_asan"
"$out/vgx_directdisplay_test_asan" | tail -1
