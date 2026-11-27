#!/usr/bin/env bash
# dread25: host test of GXV_TINYRUN's pure part (platform/vgx/src/vgx_tinyrun.h), under ASan + UBSan. Exit status 0 when every check passes.
#   tools/test_vgx_tinyrun.sh [output-dir]
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
cxx=${CXX:-g++}
nice -n 10 "$cxx" -std=gnu++20 -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-omit-frame-pointer "$root/tests/vgx_tinyrun_test.cpp" -o "$out/vgx_tinyrun_test"
UBSAN_OPTIONS=halt_on_error=1 "$out/vgx_tinyrun_test"
