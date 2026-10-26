#!/usr/bin/env bash
# Synthetic clocks only: this does not measure Vita performance.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
"${CXX:-clang++}" -std=c++17 -O1 -g -Wall -Wextra -Werror \
  -fsanitize=address,undefined -fno-omit-frame-pointer -I"$root/platform/include" \
  "$root/tests/vgx_pace_test.cpp" -o "$out/vgx_pace_test"
UBSAN_OPTIONS=halt_on_error=1 "$out/vgx_pace_test"
