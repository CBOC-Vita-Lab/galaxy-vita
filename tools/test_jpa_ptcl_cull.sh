#!/usr/bin/env bash
# GXV_HOTPROF_PTCLCULL host check (hotprof25): the real JPA units with GX as a recorder; every scenario with the switch off, on and dry; the list
# calls left out must be quads the front end's own test drops, the rest and the registers after every emitter draw identical (tests/jpa_ptcl_cull.cpp).
# The JPA units and the driver need the game's dialect (clang, as the game build); the stubs are plain C++ (g++ by default).
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d "${TMPDIR:-/tmp}/jpa-ptcl-cull.XXXXXX")}
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
"${GAMECXX:-clang++}" "${flags[@]}" -c "$root/tests/jpa_ptcl_cull.cpp" -o "$out/jpa_ptcl_cull.o"
"${CXX:-g++}" -std=c++17 -O1 -c "$root/tests/jpa_ptcl_cull_stubs.cpp" -o "$out/jpa_ptcl_cull_stubs.o"
"${CXX:-g++}" "$out/jpa_ptcl_cull.o" "${objs[@]}" "$out/jpa_ptcl_cull_stubs.o" -o "$out/jpa_ptcl_cull"
"$out/jpa_ptcl_cull"
# negative control: without the owed loads at the end of a loop the registers after an emitter draw differ: must FAIL
"${GAMECXX:-clang++}" "${flags[@]}" -DPORT_PTCLCULL_NEG=1 -c "$D/src/JSystem/JParticle/JPAResource.cpp" -o "$out/JPAResource_neg.o"
negobjs=()
for o in "${objs[@]}"; do [[ "$o" == "$out/JPAResource.o" ]] && negobjs+=("$out/JPAResource_neg.o") || negobjs+=("$o"); done
"${CXX:-g++}" "$out/jpa_ptcl_cull.o" "${negobjs[@]}" "$out/jpa_ptcl_cull_stubs.o" -o "$out/jpa_ptcl_cull_neg"
if "$out/jpa_ptcl_cull_neg" > "$out/neg.txt"; then echo "negative control PASSED (it must fail): FAIL"; exit 1; else echo "negative control fails as it must: $(grep -c '^FAIL' "$out/neg.txt") scenario runs fail"; fi
"${GAMECXX:-clang++}" "${flags[@]}" -DPORT_PTCLCULL_NEG2=1 -c "$D/src/JSystem/JParticle/JPAResource.cpp" -o "$out/JPAResource_neg2.o"
negobjs=()
for o in "${objs[@]}"; do [[ "$o" == "$out/JPAResource.o" ]] && negobjs+=("$out/JPAResource_neg2.o") || negobjs+=("$o"); done
"${CXX:-g++}" "$out/jpa_ptcl_cull.o" "${negobjs[@]}" "$out/jpa_ptcl_cull_stubs.o" -o "$out/jpa_ptcl_cull_neg2"
if "$out/jpa_ptcl_cull_neg2" > "$out/neg2.txt"; then echo "second negative control (colours never sent) PASSED (it must fail): FAIL"; exit 1; else echo "second negative control fails as it must: $(grep -c '^FAIL' "$out/neg2.txt") scenario runs fail"; fi
echo "test_jpa_ptcl_cull: PASS"
