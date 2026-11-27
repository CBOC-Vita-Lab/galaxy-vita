#!/usr/bin/env bash
# dread25: host test of GXV_ALPHATEST_PREPASS's pure parts and exactness argument (tests/vgx_prepass_test.cpp), under ASan + UBSan. Exit status 0 when every check passes.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
cxx=${CXX:-g++}
nice -n 10 "$cxx" -std=gnu++20 -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-omit-frame-pointer "$root/tests/vgx_prepass_test.cpp" -o "$out/vgx_prepass_test"
UBSAN_OPTIONS=halt_on_error=1 "$out/vgx_prepass_test"
