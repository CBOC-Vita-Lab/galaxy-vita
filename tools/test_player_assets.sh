#!/usr/bin/env bash
# luigi24: host test of the player-mode asset check (tests/player_assets.cpp, platform/include/port/player_assets.h).
#   tools/test_player_assets.sh      (host clang++ and g++; out-of-tree build in $OUT, default /tmp/player_assets)
set -euo pipefail
. "$(cd "$(dirname "$0")" && pwd)/../tests/mp/tmproot.sh"
root=$(cd "$(dirname "$0")/.." && pwd)
out=${OUT:-$MP_TMP/player_assets}
mkdir -p "$out"
cd "$root"
for cxx in clang++ g++; do
  command -v "$cxx" >/dev/null || continue
  "$cxx" -std=c++17 -O1 -Wall -Wextra -Iplatform/include tests/player_assets.cpp -o "$out/player_assets_$cxx"
  "$out/player_assets_$cxx"
done
