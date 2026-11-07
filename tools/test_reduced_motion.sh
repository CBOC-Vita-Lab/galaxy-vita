#!/usr/bin/env bash
# Reduced motion in play (platform/include/port/reduced_motion.h): the shake scale and flash cap arithmetic, off = identity, and the
# GXV_REDUCED_MOTION switch (default off, read once). No GL, no game data. Compile checks of the touched decomp units: see
# tools/check_reduced_motion_units.sh.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
nice "${CXX:-clang++}" -std=gnu++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all \
  -DTARGET_PC=1 -DVERSION=0 -fshort-wchar -fsigned-char -fno-strict-aliasing -fwrapv -fms-extensions \
  -Wno-register -Wno-multichar -Wno-writable-strings \
  -I"$root/platform/include" -isystem "$root/decomp/include" -isystem "$root/decomp/libs/JSystem/include" -isystem "$root/decomp/libs/RVL_SDK/include" \
  -include "$root/platform/include/port/compat.h" "$root/tests/reduced_motion.cpp" -o "$out/reduced_motion"
env -u GXV_REDUCED_MOTION "$out/reduced_motion" --env 0
for v in "" 0 0x0 00 invalid 1suffix " " -; do GXV_REDUCED_MOTION=$v "$out/reduced_motion" --env 0; done
for v in 1 0x1 7 -1 01; do GXV_REDUCED_MOTION=$v "$out/reduced_motion" --env 1; done
"$out/reduced_motion"
echo "reduced_motion: all passed"
