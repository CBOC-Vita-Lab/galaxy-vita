#!/bin/bash
# glow26 host test (pure logic + CPU model of the look-alike). No GL needed.
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
out=${OUT:-/tmp/glow26_test_build}; mkdir -p "$out"
${CXX:-c++} -std=c++17 -O1 -Wall -Wextra -I"$root/platform/include" "$root/tests/glow26_test.cpp" -o "$out/glow26_test"
"$out/glow26_test"
