#!/usr/bin/env bash
# gpugen26: host test of GXV_SHADOWFILL_SHRINK's maths (platform/vgx/src/vgx_fillshrink.h, tests/vgx_fillshrink_test.cpp) under ASan + UBSan. No GL needed.
#   tools/test_vgx_fillshrink.sh [output-dir]
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
nice -n 10 "${CXX:-g++}" -std=gnu++20 -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-omit-frame-pointer "$root/tests/vgx_fillshrink_test.cpp" -o "$out/vgx_fillshrink_test"
UBSAN_OPTIONS=halt_on_error=1 "$out/vgx_fillshrink_test"
