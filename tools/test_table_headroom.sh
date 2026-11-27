#!/bin/bash
# strk1118: host test, fixed-capacity settings tables at most 75 % full (tests/table_headroom.cpp)
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
out=${OUT:-/tmp/table_headroom_build}; mkdir -p "$out"
nice -n 10 ${CXX:-c++} -std=c++17 -O1 -Wall -Wextra -I"$root/platform/include" "$root/tests/table_headroom.cpp" -o "$out/table_headroom"
"$out/table_headroom"
