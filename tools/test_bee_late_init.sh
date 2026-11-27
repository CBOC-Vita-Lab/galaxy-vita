#!/usr/bin/env bash
# beefix25: host test of the Bee power-up entry rule for Mario and Luigi (tests/bee_late_init.cpp, platform/include/port/player_assets.h).
#   tools/test_bee_late_init.sh      (host clang++ and g++; out-of-tree build in $OUT, default /tmp/bee_late_init)
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${OUT:-/tmp/bee_late_init}
mkdir -p "$out"
cd "$root"
for cxx in clang++ g++; do
  command -v "$cxx" >/dev/null || continue
  "$cxx" -std=c++17 -O1 -Wall -Wextra -Iplatform/include tests/bee_late_init.cpp -o "$out/bee_late_init_$cxx"
  "$out/bee_late_init_$cxx"
done
