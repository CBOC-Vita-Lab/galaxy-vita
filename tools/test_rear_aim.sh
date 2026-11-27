#!/usr/bin/env bash
# crash33: host test of rear touch aiming's rules (tests/rear_aim.cpp, platform/include/port/rear_aim.h).
#   tools/test_rear_aim.sh      (clang++ and g++; out-of-tree build in $OUT, default /tmp/rear_aim)
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${OUT:-/tmp/rear_aim}
mkdir -p "$out"
cd "$root"
for cxx in clang++ g++; do
  command -v "$cxx" >/dev/null || continue
  "$cxx" -std=c++17 -O1 -Wall -Wextra -Werror -Iplatform/include tests/rear_aim.cpp -o "$out/rear_aim_$cxx"
  "$out/rear_aim_$cxx"
done
