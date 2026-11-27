#!/bin/bash
# deadfix27 host test for port/mode_keys.h
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
out=${OUT:-/tmp/mode_keys_test_build}; mkdir -p "$out"
${CXX:-c++} -std=c++17 -O1 -Wall -Wextra -I"$root/platform/include" "$root/tests/mode_keys_test.cpp" -o "$out/mode_keys_test"
"$out/mode_keys_test"
