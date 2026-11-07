#!/usr/bin/env bash
# GXV_AC_PANEMTX host check (animcut): tests/ac_pane_mtx.cpp with the real nw4r lyt_pane.cpp, lyt_drawInfo.cpp, ut_LinkList.cpp and
# the game's mtx.c; the same picture hash with the switch at 0, 1 and 2, and the time with 0 and 1 (NOHASH=1: CalculateMtx only).
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
D=$root/decomp
flags=(-std=gnu++17 -O1 -DTARGET_PC=1 -DNDEBUG=1 -DVERSION=0 -DPORT_LOGGING=1 -fshort-wchar -fsigned-char -fno-strict-aliasing -fwrapv
       -fms-extensions -fno-exceptions -fno-rtti -w -Wno-register -include string -include "$root/platform/include/port/compat.h"
       -I"$root/platform/include" -I"$D/include" -I"$D/libs/JSystem/include" -I"$D/libs/RVL_SDK/include" -I"$D/libs/nw4r/include")
for f in lyt/lyt_pane lyt/lyt_drawInfo ut/ut_LinkList; do
    "${GAMECXX:-clang++}" "${flags[@]}" -c "$D/src/nw4r/$f.cpp" -o "$out/${f#*/}.o"
done
"${CC:-gcc}" -O1 -I"$root/platform/include" -I"$D/libs/RVL_SDK/include" -include stdint.h -include "$root/tests/glibc_fabs_shim.h" -w \
    -c "$root/platform/src/sdk/mtx.c" -o "$out/mtx.o"
"${GAMECXX:-clang++}" "${flags[@]}" -c "$root/tests/ac_pane_mtx.cpp" -o "$out/drv.o"
# (port_log_summary is the driver's; GX and the layout allocator are never reached)
"${CXX:-g++}" "$out"/*.o -o "$out/t" -lm -Wl,--unresolved-symbols=ignore-all
h0=$(GXV_AC_PANEMTX=0 "$out/t"); h1=$(GXV_AC_PANEMTX=1 "$out/t"); h2=$(GXV_AC_PANEMTX=2 "$out/t")
echo "off: $h0"; echo "on: $h1"; echo "verify: $h2"
for m in 0 1; do s=$(date +%s%N); NOHASH=1 GXV_AC_PANEMTX=$m "$out/t" >/dev/null; echo "GXV_AC_PANEMTX=$m: $(( ($(date +%s%N) - s) / 1000 )) us"; done
[ "$h0" = "$h1" ] && [ "$h0" = "$h2" ] && echo PASS || { echo FAIL; exit 1; }
