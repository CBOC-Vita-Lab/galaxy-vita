#!/usr/bin/env bash
# GXV_JPA_LOOK host check (logic12): the real JPA units with stubbed GX, two runs per scenario (switch off / on), pictures compared.
# The JPA units and the driver need the game's dialect (clang, as the game build); the stubs are plain C++ (g++ by default).
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d "${TMPDIR:-/tmp}/jpa-look.XXXXXX")}
mkdir -p "$out"
D=$root/decomp
flags=(-std=gnu++17 -O1 -DTARGET_PC=1 -DNDEBUG=1 -DVERSION=0 -fshort-wchar -fsigned-char -fno-strict-aliasing -fwrapv -fms-extensions
       -fno-exceptions -fno-rtti -w -Wno-register -include string -include "$root/platform/include/port/compat.h"
       -I"$root/platform/include" -I"$D/include" -I"$D/libs/JSystem/include" -I"$D/libs/RVL_SDK/include" -I"$D/libs/nw4r/include")
objs=()
for f in JParticle/JPAResource JParticle/JPAParticle JParticle/JPAEmitter JParticle/JPADynamicsBlock JParticle/JPABaseShape \
         JParticle/JPAExtraShape JParticle/JPAChildShape JParticle/JPAMath JMath/JMATrigonometric JMath/JMath JSupport/JSUList; do
    "${GAMECXX:-clang++}" "${flags[@]}" -c "$D/src/JSystem/$f.cpp" -o "$out/${f#*/}.o"
    objs+=("$out/${f#*/}.o")
done
# (the game's matrix library: platform/src/sdk/mtx.c, plain C)
"${CC:-gcc}" -O1 -I"$root/platform/include" -I"$D/libs/RVL_SDK/include" -include stdint.h -include "$root/tests/glibc_fabs_shim.h" -w -c "$root/platform/src/sdk/mtx.c" -o "$out/mtx.o"
objs+=("$out/mtx.o")
"${GAMECXX:-clang++}" "${flags[@]}" -c "$root/tests/jpa_look.cpp" -o "$out/jpa_look.o"
"${CXX:-g++}" -std=c++17 -O1 -c "$root/tests/jpa_look_stubs.cpp" -o "$out/jpa_look_stubs.o"
"${CXX:-g++}" "$out/jpa_look.o" "${objs[@]}" "$out/jpa_look_stubs.o" -o "$out/jpa_look"
"$out/jpa_look"
