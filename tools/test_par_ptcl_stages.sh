#!/usr/bin/env bash
# polish23 (8 Nov): GXV_PAR_PTCL's particle storage across many stage entries, counting the C heap (tests/par_ptcl_stages.cpp, built on
# tests/par_ptcl.cpp). The real JPA units with stubbed GX, as tools/test_par_ptcl.sh. Needs alodfix23's port_jpa_par_drop (cc1305a).
#   bash tools/test_par_ptcl_stages.sh [out-dir]      env: PTCL_STAGES=<n> (60), PTCL_MODE=1|2, PTCL_OVERLAP=1, PTCL_STAGES_NODROP=1 (must FAIL)
#   (run from the patched tree; ORIG=/source/source supplies the files the patch leaves alone)
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
orig=${ORIG:-/source/source}
out=${1:-$(mktemp -d "${TMPDIR:-/tmp}/par-ptcl-stages.XXXXXX")}
mkdir -p "$out"
pick() { if [ -e "$root/$1" ]; then echo "$root/$1"; else echo "$orig/$1"; fi; }
flags=(-std=gnu++17 -O1 -DTARGET_PC=1 -DNDEBUG=1 -DVERSION=0 -fshort-wchar -fsigned-char -fno-strict-aliasing -fwrapv -fms-extensions
       -fno-exceptions -fno-rtti -w -Wno-register -include string -include "$orig/platform/include/port/compat.h"
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
# (tests/par_ptcl_stages.cpp includes tests/par_ptcl.cpp: both from the same tree)
stages=$(pick tests/par_ptcl_stages.cpp)
cp "$stages" "$out/par_ptcl_stages.cpp"
cp "$(pick tests/par_ptcl.cpp)" "$out/par_ptcl.cpp"
"${GAMECXX:-clang++}" "${flags[@]}" -c "$out/par_ptcl_stages.cpp" -o "$out/par_ptcl_stages.o"
"${CXX:-g++}" -std=c++17 -O1 -c "$(pick tests/jpa_look_stubs.cpp)" -o "$out/stubs.o"
"${CXX:-g++}" -std=c++17 -O1 -c "$(pick tests/par_ptcl_stubs.cpp)" -o "$out/stubs2.o"
"${CXX:-g++}" "$out/par_ptcl_stages.o" "${objs[@]}" "$out/stubs.o" "$out/stubs2.o" -pthread -o "$out/par_ptcl_stages"
# (glibc's per-thread cache keeps small freed blocks counted as in use: off, so the count is exact)
GLIBC_TUNABLES=glibc.malloc.tcache_count=0 "$out/par_ptcl_stages"
