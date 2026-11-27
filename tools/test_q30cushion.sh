#!/bin/bash
# q30cush31: host test of platform/include/port/q30cushion.h (tests/q30cushion.cpp: the closed forms of the fixed 30's display callback, the cushion's rules, the two chains,
# the log-trace replay, five mutants), ASan + UBSan.
set -u
root="$(cd "$(dirname "$0")/.." && pwd)"
out="${TMPDIR:-/tmp}/q30cushion_test.$$"; mkdir -p "$out"
"${CXX:-clang++}" -std=gnu++17 -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=undefined -DPORT_Q30CUSH_MUTANT -I"$root/platform/include" "$root/tests/q30cushion.cpp" -o "$out/q30cushion" || { echo "build FAILED"; rm -rf "$out"; exit 2; }
"$out/q30cushion" "$root/tests/data/q30cush_hitch_trace.txt" "$root/tests/data/q30cush_qs2_windows.txt"; rc=$?; rm -rf "$out"; exit $rc
