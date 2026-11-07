#!/bin/bash
# hivelogic (6 Nov): host proof of GXV_HL_SENSOR (SensorHitChecker.cpp) and GXV_HL_AREACUBE (AreaForm.cpp): the real units against
# their original paths on random scenarios (tests/hivelogic_cuts.cpp). Needs clang++ (the game's dialect). Prints PASS or FAIL.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d "${TMPDIR:-/tmp}/hivelogic-cuts.XXXXXX")}
mkdir -p "$out"
D=$root/decomp
flags=(-std=gnu++17 -O1 -DTARGET_PC=1 -DNDEBUG=1 -DVERSION=0 -fshort-wchar -fsigned-char -fno-strict-aliasing -fwrapv -fms-extensions
       -fno-exceptions -fno-rtti -w -Wno-register -include string -include "$root/platform/include/port/compat.h"
       -I"$root/platform/include" -I"$D/include" -I"$D/libs/JSystem/include" -I"$D/libs/RVL_SDK/include" -I"$D/libs/nw4r/include")
"${GAMECXX:-clang++}" "${flags[@]}" -c "$D/src/Game/LiveActor/SensorHitChecker.cpp" -o "$out/SensorHitChecker.o"
"${GAMECXX:-clang++}" "${flags[@]}" -c "$D/src/Game/AreaObj/AreaForm.cpp" -o "$out/AreaForm.o"
"${GAMECXX:-clang++}" "${flags[@]}" -c "$root/tests/hivelogic_cuts.cpp" -o "$out/hivelogic_cuts.o"
# (the units' other callees, never reached here, are left unresolved)
"${GAMECXX:-clang++}" "$out/hivelogic_cuts.o" "$out/SensorHitChecker.o" "$out/AreaForm.o" -no-pie -Wl,--unresolved-symbols=ignore-all -o "$out/hivelogic_cuts"
"$out/hivelogic_cuts"
