#!/usr/bin/env bash
# ta27: host test of the GXV_TA27 pure part (platform/vgx/src/vgx_ta27.h), under ASan + UBSan. Exit status 0 when every check passes.
#   tools/test_vgx_ta27.sh [output-dir]
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
cxx=${CXX:-g++}
nice -n 10 "$cxx" -std=gnu++20 -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-omit-frame-pointer "$root/tests/vgx_ta27_test.cpp" -o "$out/vgx_ta27_test"
UBSAN_OPTIONS=halt_on_error=1 "$out/vgx_ta27_test"
