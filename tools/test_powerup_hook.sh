#!/usr/bin/env bash
# crash24: host test of the PETARI_POWERUP hook's pure half (tests/powerup_hook.cpp, platform/include/port/powerup_hook.h).
#   bash tools/test_powerup_hook.sh      (host clang++ and g++; out-of-tree build in $OUT, default /tmp/powerup_hook)
set -euo pipefail
. "$(cd "$(dirname "$0")" && pwd)/../tests/mp/tmproot.sh"
root=$(cd "$(dirname "$0")/.." && pwd)
out=${OUT:-$MP_TMP/powerup_hook}
mkdir -p "$out"
cd "$root"
for cxx in clang++ g++; do
  command -v "$cxx" >/dev/null || continue
  "$cxx" -std=c++17 -O1 -Wall -Wextra -Iplatform/include tests/powerup_hook.cpp -o "$out/powerup_hook_$cxx"
  "$out/powerup_hook_$cxx"
done
