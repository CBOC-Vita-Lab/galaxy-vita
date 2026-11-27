#!/usr/bin/env bash
# GXV_PAR_PTCL host check (parwork): the real JPA units with stubbed GX, JPAEmitterManager::calcNormal serial (world S) against calcNormal on a
# real second thread (world P), after every turn the two worlds are dumped and compared. "neg" builds the negative control (a chain per
# emitter instead of per resource: must FAIL). The JPA units and the driver need the game's dialect (clang, as the game build).
# "negheap" (alodfix23) builds PortParMake without its host-heap scope: the heap-routing check must FAIL.
#   bash tools/test_par_ptcl.sh [neg|negheap] [out-dir]     (run from the patched tree; ORIG=/source/source supplies the files the patch leaves alone)
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
orig=${ORIG:-/source/source}
neg=""
if [ "${1:-}" = "neg" ]; then neg="-DPORT_PTCL_NEG_RES"; shift; fi
if [ "${1:-}" = "negheap" ]; then neg="-DPORT_PTCL_NEG_HEAP"; shift; fi
out=${1:-$(mktemp -d "${TMPDIR:-/tmp}/par-ptcl.XXXXXX")}
mkdir -p "$out"
pick() { if [ -e "$root/$1" ]; then echo "$root/$1"; else echo "$orig/$1"; fi; }
flags=(-std=gnu++17 -O1 -DTARGET_PC=1 -DNDEBUG=1 -DVERSION=0 -fshort-wchar -fsigned-char -fno-strict-aliasing -fwrapv -fms-extensions
       -fno-exceptions -fno-rtti -w -Wno-register -include string -include "$orig/platform/include/port/compat.h" $neg
       -I"$root/platform/include" -I"$root/decomp/include" -I"$root/decomp/libs/JSystem/include"
       -I"$orig/platform/include" -I"$orig/decomp/include" -I"$orig/decomp/libs/JSystem/include" -I"$orig/decomp/libs/RVL_SDK/include" -I"$orig/decomp/libs/nw4r/include")
objs=()
for f in JParticle/JPAResource JParticle/JPAParticle JParticle/JPAEmitter JParticle/JPAEmitterManager JParticle/JPADynamicsBlock JParticle/JPABaseShape \
         JParticle/JPAExtraShape JParticle/JPAChildShape JParticle/JPAMath JParticle/JPAFieldBlock JMath/JMATrigonometric JMath/JMath JSupport/JSUList; do
    "${GAMECXX:-clang++}" "${flags[@]}" -c "$(pick decomp/src/JSystem/$f.cpp)" -o "$out/${f#*/}.o"
    objs+=("$out/${f#*/}.o")
done
"${CC:-gcc}" -O1 -I"$orig/platform/include" -I"$orig/decomp/libs/RVL_SDK/include" -include stdint.h -include "$orig/tests/glibc_fabs_shim.h" -w -c "$orig/platform/src/sdk/mtx.c" -o "$out/mtx.o"
objs+=("$out/mtx.o")
"${GAMECXX:-clang++}" "${flags[@]}" -c "$(pick tests/par_ptcl.cpp)" -o "$out/par_ptcl.o"
"${CXX:-g++}" -std=c++17 -O1 -c "$orig/tests/jpa_look_stubs.cpp" -o "$out/stubs.o"
"${CXX:-g++}" -std=c++17 -O1 -c "$(pick tests/par_ptcl_stubs.cpp)" -o "$out/stubs2.o"
"${CXX:-g++}" "$out/par_ptcl.o" "${objs[@]}" "$out/stubs.o" "$out/stubs2.o" -pthread -o "$out/par_ptcl"
"$out/par_ptcl"
