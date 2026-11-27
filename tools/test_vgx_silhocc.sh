#!/usr/bin/env bash
# across26: host test of GXV_SILH_OCC's logic and glue (platform/vgx/src/vgx_silhocc.h, vgx_silhocc_gl.h with a mock GL) and of the GXV_ACROSS table
# (platform/include/port/across.h), ASan + UBSan. No GL, no game.   tools/test_vgx_silhocc.sh [output-dir]
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
nice -n 10 "${CXX:-g++}" -std=gnu++20 -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-omit-frame-pointer -I"$root/platform/include" -I"$root/platform/vgx/src" "$root/tests/vgx_silhocc_test.cpp" -o "$out/vgx_silhocc_test"
UBSAN_OPTIONS=halt_on_error=1 "$out/vgx_silhocc_test"
