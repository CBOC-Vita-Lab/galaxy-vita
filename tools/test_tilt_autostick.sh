#!/usr/bin/env bash
# relfix24: host test of the GXV_TILT_AUTOSTICK rules (tests/tilt_autostick.cpp, platform/include/port/tilt_autostick.h).
#   tools/test_tilt_autostick.sh      (clang++ and g++; out-of-tree build in $OUT, default /tmp/tilt_autostick)
set -euo pipefail
. "$(cd "$(dirname "$0")" && pwd)/../tests/mp/tmproot.sh"
root=$(cd "$(dirname "$0")/.." && pwd)
out=${OUT:-$MP_TMP/tilt_autostick}
mkdir -p "$out"
cd "$root"
for cxx in clang++ g++; do
  command -v "$cxx" >/dev/null || continue
  "$cxx" -std=c++17 -O1 -Wall -Wextra -Werror -Iplatform/include tests/tilt_autostick.cpp -o "$out/tilt_autostick_$cxx"
  env -u GXV_TILT_AUTOSTICK EXPECT_AUTOSTICK=1 "$out/tilt_autostick_$cxx"
  GXV_TILT_AUTOSTICK=1 EXPECT_AUTOSTICK=1 "$out/tilt_autostick_$cxx" >/dev/null
  GXV_TILT_AUTOSTICK=0 EXPECT_AUTOSTICK=0 "$out/tilt_autostick_$cxx" >/dev/null
done
