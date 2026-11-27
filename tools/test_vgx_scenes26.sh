#!/usr/bin/env bash
# scenes26: host test of the depth load/store decision table, the tail predictor, the A/B schedule and the census (platform/vgx/src/vgx_scenes26.h,
# tests/vgx_scenes26_test.cpp) under ASan + UBSan. No GL needed.
#   tools/test_vgx_scenes26.sh [output-dir]
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
nice -n 10 "${CXX:-g++}" -std=gnu++20 -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-omit-frame-pointer "$root/tests/vgx_scenes26_test.cpp" -o "$out/vgx_scenes26_test"
UBSAN_OPTIONS=halt_on_error=1 "$out/vgx_scenes26_test"
