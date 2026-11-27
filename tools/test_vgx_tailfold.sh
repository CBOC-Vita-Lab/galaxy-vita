#!/usr/bin/env bash
# Host test of the pure logic of GXV_TAILFOLD (bloom37 / tail37): the decision, the census and the log line (tests/vgx_tailfold_test.cpp), plain and under ASan + UBSan. No GL, no SDK.
#   tools/test_vgx_tailfold.sh [output-dir]
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
nice -n 10 "${CXX:-g++}" -std=gnu++20 -O1 -g -Wall -Wextra -I"$root/platform/vgx/src" "$root/tests/vgx_tailfold_test.cpp" -o "$out/vgx_tailfold_test"
"$out/vgx_tailfold_test"
nice -n 10 "${CXX:-g++}" -std=gnu++20 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined -Wall -Wextra -I"$root/platform/vgx/src" "$root/tests/vgx_tailfold_test.cpp" -o "$out/vgx_tailfold_test_asan"
"$out/vgx_tailfold_test_asan" | tail -1
