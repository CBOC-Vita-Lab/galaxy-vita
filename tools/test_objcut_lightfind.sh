#!/bin/bash
# objcut (7 Nov): host proof of GXV_OC_LIGHTFIND (LightAreaHolder::portFindIn) against AreaObjMgr::find_in, real units. Prints PASS or FAIL.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d "${TMPDIR:-/tmp}/objcut-lightfind.XXXXXX")}
mkdir -p "$out"
D=$root/decomp
flags=(-std=gnu++17 -O1 -DTARGET_PC=1 -DNDEBUG=1 -DVERSION=0 -fshort-wchar -fsigned-char -fno-strict-aliasing -fwrapv -fms-extensions
       -fno-exceptions -fno-rtti -w -Wno-register -include string -include "$root/platform/include/port/compat.h"
       -I"$root/platform/include" -I"$D/include" -I"$D/libs/JSystem/include" -I"$D/libs/RVL_SDK/include" -I"$D/libs/nw4r/include")
for f in AreaObj/LightAreaHolder AreaObj/AreaObj AreaObj/AreaForm AreaObj/LightArea; do
  "${GAMECXX:-clang++}" "${flags[@]}" -c "$D/src/Game/$f.cpp" -o "$out/$(basename $f).o"
done
"${GAMECXX:-clang++}" "${flags[@]}" -c "$root/tests/objcut_lightfind.cpp" -o "$out/test.o"
"${GAMECXX:-clang++}" "$out"/*.o -no-pie -Wl,--unresolved-symbols=ignore-all -o "$out/objcut_lightfind"
"$out/objcut_lightfind" 1
"$out/objcut_lightfind" 2
