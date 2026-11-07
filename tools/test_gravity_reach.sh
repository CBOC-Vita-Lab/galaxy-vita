#!/usr/bin/env bash
# PETARI_GRAVITY_CULL (ported from Producdevity's fork): tests/gravity_reach.cpp with the real point, disk and cube gravity sources
# (UBSan): ~2 M positions around each field's limit and far from it must give the same result with the reach test as without, bit
# for bit; then one mutant of the cube's bound (an axis left out) must fail it. Host (x86-64 or ARM) build; no console needed.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d "${TMPDIR:-/tmp}/gravity-reach.XXXXXX")}
mkdir -p "$out"
"${CC:-gcc}" -O1 -I"$root/platform/include" -I"$root/decomp/libs/RVL_SDK/include" -include stdint.h -include "$root/tests/glibc_fabs_shim.h" -w \
    -c "$root/platform/src/sdk/mtx.c" -o "$out/mtx.o"
echo 'extern "C" { unsigned port_grav_gen = 1; }' > "$out/stub.cpp"  # (GXV_MO_GRAVMEMO's generation, written by PlanetGravity.cpp)
build() {  # decomp tree, output
    "${CXX:-clang++}" -std=c++20 -w -Wno-register -O2 -g -fshort-wchar -fsigned-char -fms-extensions -DTARGET_PC=1 -fno-strict-aliasing -fwrapv \
        -ffp-contract=on -include "$root/vita/include/vita_source_prelude.h" -include "$root/platform/include/port/compat.h" \
        -I"$root/platform/include" -I"$1/include" -I"$root/decomp/libs/JSystem/include" -I"$root/decomp/libs/RVL_SDK/include" \
        -I"$root/decomp/libs/nw4r/include" -I"$root/decomp/libs/RVLFaceLib/include" -fno-exceptions -fno-rtti \
        -fsanitize=undefined -fno-omit-frame-pointer -fno-pie -no-pie \
        "$root/tests/gravity_reach.cpp" "$out/stub.cpp" "$1"/src/Game/Gravity/{PlanetGravity,PointGravity,DiskGravity,CubeGravity}.cpp \
        "$root/decomp/src/Game/Util/MathUtil.cpp" "$root/decomp/src/JSystem/JMath/JMATrigonometric.cpp" "$root/decomp/src/JSystem/JMath/JMath.cpp" \
        "$out/mtx.o" -lm -Wl,--unresolved-symbols=ignore-all -o "$2"
}
build "$root/decomp" "$out/gravity_reach"
UBSAN_OPTIONS=halt_on_error=1 "$out/gravity_reach" 20000

rm -rf "$out/m" && mkdir -p "$out/m" && cp -r "$root/decomp/include" "$root/decomp/src" "$out/m/"
sed -i -e "s/(lenX + lenY + lenZ + mRange/(lenX + lenY + mRange/" "$out/m/src/Game/Gravity/CubeGravity.cpp"
build "$out/m" "$out/gravity_mutant"
if "$out/gravity_mutant" 20000 > "$out/mutant.log"; then echo "FAIL: the cube-axis mutant survived"; cat "$out/mutant.log"; exit 1; fi
echo "mutant cube-axis caught: $(tail -1 "$out/mutant.log")"
echo "PASS: gravity reach test exact"
