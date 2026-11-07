#!/usr/bin/env bash
# GXV_FIFO_BATCH host proof: the production GX units and J3DGD.cpp, batched against element-wise (tests/fifo_batch.cpp).
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
for f in GXLight GXTransform GXDisplayList GXGeometry GXAttr; do
    "${CC:-clang}" "${c[@]}" -c "$root/decomp/src/RVL_SDK/gx/$f.c" -o "$out/$f.o"
    objs+=("$out/$f.o")
done
"${CXX:-clang++}" "${cxx[@]}" -c "$root/decomp/src/JSystem/J3DGraphBase/J3DGD.cpp" -o "$out/J3DGD.o"
# (the C units each carry the header's out-of-line helpers: --allow-multiple-definition, as one TU each would in the game)
"${CXX:-clang++}" "${cxx[@]}" "$root/tests/fifo_batch.cpp" "${objs[@]}" "$out/J3DGD.o" -Wl,--gc-sections -Wl,--allow-multiple-definition \
    -o "$out/fifo_batch"
"$out/fifo_batch"
