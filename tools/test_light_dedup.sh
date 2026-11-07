#!/usr/bin/env bash
# GXV_LIGHT_DEDUP host proof: the production GXLight.c / GXDisplayList.c and port/light_memo.h (tests/light_dedup.cpp).
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
inc=(-I"$root/platform/include" -I"$root/decomp/include" -I"$root/decomp/libs/RVL_SDK/include" -I"$root/decomp/libs/JSystem/include")
c=(-O2 -fno-strict-aliasing -fms-extensions -DTARGET_PC=1 "${inc[@]}" -include "$root/platform/include/port/compat.h" -w
   -ffunction-sections -fdata-sections -Wno-implicit-function-declaration)
cxx=(-std=c++17 -O2 -fno-strict-aliasing -fms-extensions -DTARGET_PC=1 "${inc[@]}" -include string -include vector
     -include "$root/platform/include/port/compat.h" -w -ffunction-sections -fdata-sections)
objs=()
for f in GXLight GXDisplayList; do
    "${CC:-clang}" "${c[@]}" -c "$root/decomp/src/RVL_SDK/gx/$f.c" -o "$out/$f.o"
    objs+=("$out/$f.o")
done
"${CXX:-clang++}" "${cxx[@]}" "$root/tests/light_dedup.cpp" "${objs[@]}" -Wl,--gc-sections -Wl,--allow-multiple-definition \
    -o "$out/light_dedup"
"$out/light_dedup"
