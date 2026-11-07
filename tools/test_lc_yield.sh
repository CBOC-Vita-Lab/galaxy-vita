#!/usr/bin/env bash
# GXV_LC_YIELD host proof: the real scheduler with real guest threads, switch unset and 1; the traces must be identical
# (tests/lc_yield.cpp). No console, no Vita timing.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
"${CXX:-clang++}" -std=c++20 -O1 -g -DTARGET_PC=1 -DNDEBUG=1 -fms-extensions -ffunction-sections -Wl,--gc-sections \
  -I"$root/platform/include" -I"$root/decomp/libs/RVL_SDK/include" -include string -include "$root/platform/include/port/compat.h" \
  -Wno-register -Wno-unknown-pragmas -Wno-deprecated-volatile "$root/tests/lc_yield.cpp" -o "$out/lc_yield" -pthread
a=$(env -u GXV_LC_YIELD timeout 120 "$out/lc_yield")
b=$(GXV_LC_YIELD=1 timeout 120 "$out/lc_yield")
echo "$a"; echo "$b"
ha=$(echo "$a" | sed -n 's/.*hash \([0-9a-f]*\);.*/\1/p'); hb=$(echo "$b" | sed -n 's/.*hash \([0-9a-f]*\);.*/\1/p')
kept=$(echo "$b" | sed -n 's/.*yields, \([0-9]*\) kept.*/\1/p')
if [[ -n $ha && $ha == "$hb" && ${kept:-0} -gt 0 ]]; then echo PASS; else echo FAIL; exit 1; fi
