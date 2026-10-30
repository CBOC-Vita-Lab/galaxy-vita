#!/usr/bin/env bash
# Thread-layout policy (platform/include/port/core_layout.h) against a fake kernel: no console, no Vita timing.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
"${CXX:-clang++}" -std=c++17 -O1 -g -Wall -Wextra -Werror \
  -fsanitize=address,undefined -fno-omit-frame-pointer -I"$root/platform/include" \
  "$root/tests/core_layout_test.cpp" -o "$out/core_layout_test"
UBSAN_OPTIONS=halt_on_error=1 "$out/core_layout_test"
