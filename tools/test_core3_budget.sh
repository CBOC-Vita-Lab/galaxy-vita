#!/usr/bin/env bash
# core3-38: the core-3 yield cap and meter (platform/include/port/core3_budget.h): pure logic on a PC.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
"${CXX:-clang++}" -std=c++17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer -pthread \
  -I"$root/platform/include" "$root/tests/core3_budget_test.cpp" -o "$out/core3_budget_test"
UBSAN_OPTIONS=halt_on_error=1 "$out/core3_budget_test"
