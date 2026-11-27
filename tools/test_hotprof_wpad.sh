#!/usr/bin/env bash
# GXV_HOTPROF_WPAD host check (hotprof25 round 2): the game's WPadAcceleration.cpp with the switch at 0, 1 and 2 on the same samples
# (still stretches around and beyond the history's length, movement, NaN / infinity / signed zero / denormal samples, turns with no or
# several samples, a changed rotate window, an object rebuilt in place): every field bit-equal after every update (tests/hotprof_wpad.cpp).
# With -DPORT_WPAD_NEG=1 the run of equal samples is not reset by a different sample: the check must then FAIL.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${2:-$(mktemp -d "${TMPDIR:-/tmp}/hotprof-wpad.XXXXXX")}
mkdir -p "$out"
D=$root/decomp
flags=(-std=gnu++17 -O1 -DTARGET_PC=1 -DNDEBUG=1 -DVERSION=0 -fshort-wchar -fsigned-char -fno-strict-aliasing -fwrapv -fms-extensions
       -fno-exceptions -fno-rtti -w -Wno-register -include string -include "$root/platform/include/port/compat.h"
       -I"$root/platform/include" -I"$D/include" -I"$D/libs/JSystem/include" -I"$D/libs/RVL_SDK/include" -I"$D/libs/nw4r/include")
cxx=${GAMECXX:-clang++}
"$cxx" "${flags[@]}" -c "$D/src/Game/System/WPadAcceleration.cpp" -o "$out/wpadacc.o"
"$cxx" "${flags[@]}" -c "$root/tests/hotprof_wpad.cpp" -o "$out/test.o"
"$cxx" "$out/test.o" "$out/wpadacc.o" -o "$out/hotprof_wpad" -lm
"$out/hotprof_wpad" "${1:-200}"
"$cxx" "${flags[@]}" -DPORT_WPAD_NEG=1 -c "$D/src/Game/System/WPadAcceleration.cpp" -o "$out/wpadacc_neg.o"
"$cxx" "$out/test.o" "$out/wpadacc_neg.o" -o "$out/hotprof_wpad_neg" -lm
if "$out/hotprof_wpad_neg" "${1:-200}" > "$out/neg.txt"; then echo "negative control PASSED (it must fail): FAIL"; exit 1; else echo "negative control fails as it must: $(tail -3 "$out/neg.txt" | head -1)"; fi
echo "test_hotprof_wpad: PASS"
