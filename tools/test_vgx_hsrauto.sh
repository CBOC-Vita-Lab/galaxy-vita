#!/usr/bin/env bash
# geom26: host test of GXV_HSR_AUTO's pure part (platform/vgx/src/vgx_hsrauto.h), under ASan + UBSan. Exit status 0 when every check passes.
#   tools/test_vgx_hsrauto.sh [output-dir]
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
cxx=${CXX:-g++}
nice -n 10 "$cxx" -std=gnu++20 -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-omit-frame-pointer "$root/tests/vgx_hsrauto_test.cpp" -o "$out/vgx_hsrauto_test"
UBSAN_OPTIONS=halt_on_error=1 "$out/vgx_hsrauto_test"
