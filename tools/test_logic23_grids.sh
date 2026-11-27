#!/bin/bash
# logic23 (8 Nov): host proof of GXV_GRAV_GRID (PlanetGravityManager.cpp), GXV_AREA_GRID (AreaObj.cpp find_in) and its manager-name memo
# (AreaObjContainer.cpp getManager), with the real units (tests/logic23_grids.cpp). Each switch at 0, 1 and 2 must give the same hash
# of every answer; 2 must count no mismatch; the grids must answer most lookups. Then the negative control (PORT_LGRID_NEG: boxes
# filed too small, the memo blind to the text) must FAIL. `bash tools/test_logic23_grids.sh [outdir] [asan|tsan]` (asan: ASan + UBSan;
# tsan: ThreadSanitizer, with a second thread making area lookups and manager searches beside the game thread).
# Needs clang++. Prints PASS or FAIL.
set -euo pipefail
root=${LG_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}
out=${1:-$(mktemp -d "${TMPDIR:-/tmp}/logic23-grids.XXXXXX")}
mkdir -p "$out"
D=$root/decomp
san=()
if [ "${2:-}" = "asan" ]; then
    san=(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize=alignment,vptr)
    export ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1  # (the test never frees its scene)
fi
if [ "${2:-}" = "tsan" ]; then
    san=(-fsanitize=thread -fno-omit-frame-pointer)
    export TSAN_OPTIONS=halt_on_error=1 THREADS=1
fi
flags=(-std=gnu++17 -O1 -DTARGET_PC=1 -DNDEBUG=1 -DVERSION=0 -fshort-wchar -fsigned-char -fno-strict-aliasing -fwrapv -fms-extensions
       -fno-exceptions -fno-rtti -w -Wno-register -include string -include "$root/platform/include/port/compat.h"
       -I"$root/platform/include" -I"$D/include" -I"$D/libs/JSystem/include" -I"$D/libs/RVL_SDK/include" -I"$D/libs/nw4r/include" "${san[@]}")
units=(decomp/src/Game/Gravity/PlanetGravityManager.cpp decomp/src/Game/Gravity/PlanetGravity.cpp decomp/src/Game/Gravity/PointGravity.cpp
       decomp/src/Game/Gravity/DiskGravity.cpp decomp/src/Game/Gravity/SegmentGravity.cpp decomp/src/Game/Gravity/GravityInfo.cpp
       decomp/src/Game/Util/MathUtil.cpp decomp/src/Game/Util/MtxUtil.cpp decomp/src/Game/AreaObj/AreaObj.cpp decomp/src/Game/AreaObj/AreaForm.cpp
       decomp/src/Game/AreaObj/AreaObjContainer.cpp)
build() {  # $1: tag, $2...: extra flags
    local tag=$1; shift
    local objs=()
    for f in "${units[@]}" tests/logic23_grids.cpp; do
        local o=$out/$tag-$(basename "$f" .cpp).o
        "${GAMECXX:-clang++}" "${flags[@]}" "$@" -c "$root/$f" -o "$o"
        objs+=("$o")
    done
    "${CC:-clang}" -O1 -I"$root/platform/include" -I"$D/libs/RVL_SDK/include" -include stdint.h -include "$root/tests/glibc_fabs_shim.h" -w \
        "${san[@]}" -c "$root/platform/src/sdk/mtx.c" -o "$out/$tag-mtx.o"
    "${GAMECXX:-clang++}" "${flags[@]}" -c "$D/src/JSystem/JMath/JMATrigonometric.cpp" -o "$out/$tag-JMATrigonometric.o"
    objs+=("$out/$tag-mtx.o" "$out/$tag-JMATrigonometric.o")
    # (with asan: linked by g++, whose ASan / UBSan runtimes are installed here; clang's are not)
    "${LINKCXX:-$([ ${#san[@]} -gt 0 ] && echo g++ || echo "${GAMECXX:-clang++}")}" "${san[@]}" "${objs[@]}" -lm -no-pie \
        -Wl,--unresolved-symbols=ignore-all -o "$out/logic23_grids_$tag"
}
build pos
h() { echo "$1" | sed -n 's/^hash \([0-9a-f]*\).*/\1/p'; }
ml() { echo "$1" | sed -n 's/^hash [0-9a-f]* mismatch-lines \([0-9]*\).*/\1/p'; }
ok=1
r0=$(GXV_GRAV_GRID=0 GXV_AREA_GRID=0 GXV_MO_GRAVMEMO=1 "$out/logic23_grids_pos" all) || ok=0
echo "== 0 (off):"; echo "$r0"
r1=$(GXV_GRAV_GRID=1 GXV_AREA_GRID=1 GXV_MO_GRAVMEMO=1 "$out/logic23_grids_pos" all) || ok=0
echo "== 1 (on):"; echo "$r1"
r2=$(GXV_GRAV_GRID=2 GXV_AREA_GRID=2 GXV_MO_GRAVMEMO=1 "$out/logic23_grids_pos" all) || ok=0
echo "== 2 (verify):"; echo "$r2"
[ -n "$(h "$r0")" ] && [ "$(h "$r0")" = "$(h "$r1")" ] && [ "$(h "$r0")" = "$(h "$r2")" ] || { echo "hash differs"; ok=0; }
[ "$(ml "$r2")" = 0 ] || { echo "mismatch lines in verify"; ok=0; }
echo "$r2" | grep -q "mismatches 0 (GXV_GRAV_GRID=2)" || { echo "grav-grid verify not clean"; ok=0; }
echo "$r2" | grep -q "^area-grid mismatches 0 memo-mismatches 0" || { echo "area-grid verify not clean"; ok=0; }
# the grids must answer (not just step aside)
echo "$r1" | awk '/^totals/ { split($0, a, /through the grid /); g = a[2] + 0; b = a[3] + 0; if (g < 100000 || b < 100000) exit 1 }' || { echo "grids did not answer"; ok=0; }
# the cache-free walk (memo off) must give the same answers as the memo + grid one for the area part (gravity: the memo is needed)
ra=$(GXV_AREA_GRID=0 GXV_HL_AREACUBE=0 "$out/logic23_grids_pos" area); rb=$(GXV_AREA_GRID=1 "$out/logic23_grids_pos" area)
[ "$(h "$ra")" = "$(h "$rb")" ] || { echo "area: grid vs plain walk without the cube boxes differ"; ok=0; }
if [ -z "${2:-}" ]; then
    build neg -DPORT_LGRID_NEG=1
    n1=$(GXV_GRAV_GRID=1 GXV_AREA_GRID=1 GXV_MO_GRAVMEMO=1 "$out/logic23_grids_neg" all) || true
    n2=$(GXV_GRAV_GRID=2 GXV_AREA_GRID=2 GXV_MO_GRAVMEMO=1 "$out/logic23_grids_neg" all) || true
    echo "== negative control (2):"; echo "$n2" | tail -4
    if [ "$(h "$n1")" = "$(h "$r0")" ] || [ "$(ml "$n2")" = 0 ]; then echo "NEGATIVE CONTROL DID NOT FAIL"; ok=0; else echo "negative control failed as it must"; fi
fi
[ $ok = 1 ] && echo PASS || { echo FAIL; exit 1; }
