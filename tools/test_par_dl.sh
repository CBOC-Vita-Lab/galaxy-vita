#!/usr/bin/env bash
# GXV_PAR_DL (gamedraw23) on the host: tests/par_dl.cpp with the production material path (J3DMaterial / J3DMatBlock / J3DTevs / J3DGD /
# J3DPacket / J3DTransform / J3DStruct, GDBase.c, mtx.c; TARGET_PC: J3D_MSYS), port/par_dl.h and Game/LiveActor/ParDlSnap.hpp.
# ThreadSanitizer and AddressSanitizer+UBSan builds; every scenario must match the serial world turn by turn with no report; the negative
# controls must fail. (clang compiles the decomp headers; the sanitizer runtimes are the compiler's own.)
#   bash tools/test_par_dl.sh [turns] [seeds]       (OUT: build directory, default a temporary one)
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
turns=${1:-60}
seeds=${2:-"1 2 3"}
out=${OUT:-$(mktemp -d)}
CXX=${CXX:-clang++}
CC=${CC:-clang}
units=(JSystem/J3DGraphBase/J3DMaterial JSystem/J3DGraphBase/J3DMatBlock JSystem/J3DGraphBase/J3DTevs JSystem/J3DGraphBase/J3DGD
       JSystem/J3DGraphBase/J3DPacket JSystem/J3DGraphBase/J3DTransform JSystem/J3DGraphBase/J3DStruct JSystem/JMath/JMATrigonometric)
build() {  # $1: name, $2..: sanitizer flags
    local name=$1; shift
    local common=(-g -O1 -Wno-register -DTARGET_PC=1 -fno-strict-aliasing -w -fno-omit-frame-pointer "$@" -include stdint.h
                  -include "$root/tests/glibc_fabs_shim.h" -I"$root/platform/include" -I"$root/decomp/libs/JSystem/include"
                  -I"$root/decomp/libs/RVL_SDK/include" -I"$root/decomp/include")
    local objs=()
    for u in "${units[@]}"; do
        local o="$out/$name-$(basename "$u").o"
        "$CXX" -std=c++20 -fms-extensions -include cfloat "${common[@]}" -c "$root/decomp/src/$u.cpp" -o "$o"
        objs+=("$o")
    done
    "$CC" -x c -Wno-implicit-function-declaration "${common[@]}" -c "$root/decomp/src/RVL_SDK/gd/GDBase.c" -o "$out/$name-gdbase.o"
    "$CC" -x c -Wno-implicit-function-declaration "${common[@]}" -c "$root/platform/src/sdk/mtx.c" -o "$out/$name-mtx.o"
    "$CXX" -std=c++20 -fms-extensions -include cfloat "${common[@]}" "$root/tests/par_dl.cpp" "${objs[@]}" "$out/$name-gdbase.o" "$out/$name-mtx.o" \
        -pthread -lm ${RTLINK[$name]} -o "$out/par_dl-$name"
}
# (the sanitizer runtimes: clang's when installed, else GCC's, as tools/test_dl_lazy_retarget.sh links them)
declare -A RTLINK=([tsan]="" [asan]="")
if ! echo 'int main(){}' | "$CXX" -x c++ -fsanitize=thread - -o "$out/rt-probe" 2>/dev/null; then
    RTLINK=([tsan]="-fno-sanitize-link-runtime -l:libtsan.so.2" [asan]="-fno-sanitize-link-runtime -l:libasan.so.8 -l:libubsan.so.1")
fi
build tsan -fsanitize=thread
build asan -fsanitize=address,undefined -fno-sanitize=vptr,alignment -fno-sanitize-recover=undefined
export PDL_VIEW_WORK=${PDL_VIEW_WORK:-400} TSAN_OPTIONS="halt_on_error=1 exitcode=66" ASAN_OPTIONS=detect_leaks=0:verify_asan_link_order=0 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
fail=0
for seed in $seeds; do
    for scen in plain early foreign player take slow verify; do
        for b in tsan asan; do
            if ! "$out/par_dl-$b" run "$turns" "$seed" "$scen" > "$out/run.txt" 2> "$out/run.err"; then
                echo "FAIL: $b seed $seed $scen"; tail -20 "$out/run.err"; fail=1
            else
                echo "$b: $(cat "$out/run.txt")"
            fi
        done
    done
done
for b in tsan asan; do
    if ! "$out/par_dl-$b" help 400 > "$out/run.txt" 2> "$out/run.err"; then
        echo "FAIL: $b help"; tail -20 "$out/run.err"; fail=1
    else
        echo "$b: $(cat "$out/run.txt")"
    fi
done
# negative controls: each must fail (a ThreadSanitizer report, exit 66, or a difference from the serial world)
for neg in "sys plain" "merge plain" "enter early" "verify verify" "sys player" "merge player" "playerin player"; do
    set -- $neg
    if PDL_NEG=$1 "$out/par_dl-tsan" run "$turns" 1 "$2" > "$out/neg.txt" 2> "$out/neg.err"; then
        echo "FAIL: negative control PDL_NEG=$1 ($2) passed"; cat "$out/neg.txt"; fail=1
    else
        why=$(grep -m1 -o "WARNING: ThreadSanitizer: [a-z -]*" "$out/neg.err" || true)
        echo "negative control PDL_NEG=$1 ($2): fails as it must ($(tail -1 "$out/neg.txt" 2>/dev/null)${why:+; $why})"
    fi
done
# the verify flow (GXV_PAR_DL=2's) finds a broken overlap by itself, without ThreadSanitizer
if PDL_NEG=verify "$out/par_dl-asan" run "$turns" 1 verify > "$out/neg.txt" 2> /dev/null || ! grep -q "VERIFY FOUND DIFFERENCES" "$out/neg.txt"; then
    echo "FAIL: the verify flow did not report the broken overlap"; cat "$out/neg.txt"; fail=1
else
    echo "negative control PDL_NEG=verify (AddressSanitizer build, verify flow): $(tail -1 "$out/neg.txt")"
fi
[ $fail = 0 ] && echo "test_par_dl: PASS" || { echo "test_par_dl: FAIL"; exit 1; }
