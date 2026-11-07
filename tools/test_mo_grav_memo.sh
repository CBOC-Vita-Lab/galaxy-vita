#!/bin/bash
# mapobjcut (6 Nov): host proof of GXV_MO_GRAVMEMO (PlanetGravityManager.cpp) with the real gravity units (tests/mo_grav_memo.cpp).
# Needs clang++. Prints PASS or FAIL.
set -euo pipefail
root=${MO_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}  # (MO_ROOT: the full tree, when this runs from a sparse copy)
src=${MO_SRC:-$root}  # (a sparse copy can put its own files first: MO_SRC=<sparse root>, root = the full tree)
out=${1:-$(mktemp -d "${TMPDIR:-/tmp}/mo-grav-memo.XXXXXX")}
mkdir -p "$out"
D=$root/decomp
flags=(-std=gnu++17 -O1 -DTARGET_PC=1 -DNDEBUG=1 -DVERSION=0 -fshort-wchar -fsigned-char -fno-strict-aliasing -fwrapv -fms-extensions
       -fno-exceptions -fno-rtti -w -Wno-register -include string -include "$root/platform/include/port/compat.h"
       -I"$src/platform/include" -I"$src/decomp/include" -I"$root/platform/include" -I"$D/include" -I"$D/libs/JSystem/include"
       -I"$D/libs/RVL_SDK/include" -I"$D/libs/nw4r/include")
pick() { if [ -f "$src/$1" ]; then echo "$src/$1"; else echo "$root/$1"; fi; }
objs=()
for f in decomp/src/Game/Gravity/PlanetGravityManager.cpp decomp/src/Game/Gravity/PlanetGravity.cpp decomp/src/Game/Gravity/PointGravity.cpp \
         decomp/src/Game/Gravity/DiskGravity.cpp decomp/src/Game/Gravity/SegmentGravity.cpp decomp/src/Game/Gravity/GravityInfo.cpp \
         decomp/src/Game/Util/MathUtil.cpp tests/mo_grav_memo.cpp; do
    o=$out/$(basename "$f" .cpp).o
    "${GAMECXX:-clang++}" "${flags[@]}" -c "$(pick "$f")" -o "$o"
    objs+=("$o")
done
# (host vector / trigonometry helpers the gravities reach: the port's C mtx library and JMath's tables)
"${CC:-gcc}" -O1 -I"$root/platform/include" -I"$D/libs/RVL_SDK/include" -include stdint.h -include "$root/tests/glibc_fabs_shim.h" -w \
    -c "$root/platform/src/sdk/mtx.c" -o "$out/mtx.o"  # (as tools/test_jpa_look.sh)
"${GAMECXX:-clang++}" "${flags[@]}" -c "$D/src/JSystem/JMath/JMATrigonometric.cpp" -o "$out/JMATrigonometric.o"
objs+=("$out/mtx.o" "$out/JMATrigonometric.o")
"${GAMECXX:-clang++}" "${objs[@]}" -lm -no-pie -Wl,--unresolved-symbols=ignore-all -o "$out/mo_grav_memo"
r0=$(GXV_MO_GRAVMEMO=0 "$out/mo_grav_memo"); echo "off:    $r0"
r1=$(GXV_MO_GRAVMEMO=1 "$out/mo_grav_memo"); echo "on:     $r1"
r2=$(GXV_MO_GRAVMEMO=2 "$out/mo_grav_memo"); echo "verify: $r2"
rm1=$(MUTATE=1 GXV_MO_GRAVMEMO=0 "$out/mo_grav_memo"); echo "mutated off:    $rm1"
rm2=$(MUTATE=1 GXV_MO_GRAVMEMO=1 "$out/mo_grav_memo"); echo "mutated on:     $rm2"
rm3=$(MUTATE=1 GXV_MO_GRAVMEMO=2 "$out/mo_grav_memo"); echo "mutated verify: $rm3"
h() { echo "$1" | sed 's/.*hash \([0-9a-f]*\).*/\1/'; }
mm() { echo "$1" | sed 's/.*mismatches \([0-9]*\).*/\1/'; }
ok=1
[ "$(h "$r0")" = "$(h "$r1")" ] && [ "$(h "$r0")" = "$(h "$r2")" ] || ok=0
[ "$(mm "$r2")" = 0 ] || ok=0
[ "$(mm "$rm3")" != 0 ] || ok=0                  # the verify check sees a write that does not move the generation
[ "$(h "$rm1")" != "$(h "$rm2")" ] || ok=0       # and the memo would then differ: the generation is what keeps it exact
[ $ok = 1 ] && echo PASS || { echo FAIL; exit 1; }
