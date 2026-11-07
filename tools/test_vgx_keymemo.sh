#!/usr/bin/env bash
# Host-side test of the shader-key memo table of the resolution cache (platform/vgx/src/vgx_keymemo.h): every lookup resolves to the
# value the miss path computes, whatever the associativity; set-associativity only removes conflict misses.
#   tools/test_vgx_keymemo.sh [output-dir]      (default: a temporary directory)
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
cxx=${CXX:-g++}
nice -n 10 "$cxx" -std=c++17 -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I"$root/platform/vgx/src" "$root/tests/vgx_keymemo_test.cpp" -o "$out/vgx_keymemo_test"
UBSAN_OPTIONS=halt_on_error=1 "$out/vgx_keymemo_test"
