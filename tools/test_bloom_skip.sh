#!/usr/bin/env bash
# PETARI_BLOOM_SKIP host proof, GX side (tests/bloom_skip_serial.cpp): the production GX units' EFB-writer counter.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
base=${BASE_ROOT:-$root}   # (a sparse tree: units it lacks come from BASE_ROOT)
pick() { if [ -f "$root/$1" ]; then echo "$root/$1"; else echo "$base/$1"; fi; }
out=${OUT:-$(mktemp -d)}
[ -z "${OUT:-}" ] && trap 'rm -rf "$out"' EXIT
inc=(-I"$base/platform/include" -I"$base/decomp/include" -I"$base/decomp/libs/RVL_SDK/include" -I"$base/decomp/libs/JSystem/include")
c=(-O2 -fno-strict-aliasing -fms-extensions -DTARGET_PC=1 "${inc[@]}" -include "$base/platform/include/port/compat.h" -w
   -ffunction-sections -fdata-sections -Wno-implicit-function-declaration)
cxx=(-std=c++17 -O2 -fno-strict-aliasing -fms-extensions -DTARGET_PC=1 "${inc[@]}" -include string -include vector -include "$base/platform/include/port/compat.h" -w)
objs=()
for f in GXTransform GXDisplayList GXGeometry GXFrameBuf GXTexture; do
    "${CC:-clang}" "${c[@]}" -c "$(pick decomp/src/RVL_SDK/gx/$f.c)" -o "$out/$f.o"
    objs+=("$out/$f.o")
done
"${CC:-clang}" -c -w "$root/tests/bloom_skip_gxcap_stubs.c" -o "$out/gxcap_stubs.o"   # (tests31: the GX capture layer's symbols, idle here)
"${CXX:-clang++}" "${cxx[@]}" "$(pick tests/bloom_skip_serial.cpp)" "${objs[@]}" "$out/gxcap_stubs.o" -Wl,--gc-sections -Wl,--allow-multiple-definition -o "$out/bloom_skip_serial"
"$out/bloom_skip_serial"
