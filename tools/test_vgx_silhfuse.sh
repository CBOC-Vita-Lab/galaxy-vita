#!/usr/bin/env bash
# silhfuse26: host test of GXV_SILH_FUSE's arithmetic and pairing (platform/vgx/src/vgx_silhfuse.h, tests/vgx_silhfuse_test.cpp) under ASan + UBSan. No GL needed.
#   tools/test_vgx_silhfuse.sh [output-dir]
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
nice -n 10 "${CXX:-g++}" -std=gnu++20 -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-omit-frame-pointer "$root/tests/vgx_silhfuse_test.cpp" -o "$out/vgx_silhfuse_test"
UBSAN_OPTIONS=halt_on_error=1 "$out/vgx_silhfuse_test"
