#!/usr/bin/env bash
# scenes28: host test of the pure part (platform/vgx/src/vgx_scenes28.h), under ASan + UBSan.   tools/test_vgx_scenes28.sh [output-dir]
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
cxx=${CXX:-g++}
nice -n 15 "$cxx" -std=gnu++20 -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-omit-frame-pointer "$root/tests/vgx_scenes28_test.cpp" -o "$out/vgx_scenes28_test"
UBSAN_OPTIONS=halt_on_error=1 "$out/vgx_scenes28_test"
