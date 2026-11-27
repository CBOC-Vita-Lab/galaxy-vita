#!/usr/bin/env bash
# GXV_REPLAY25 (replay25, 12 Nov) on the host: tests/draw_replay25.cpp with the production draw buffers (Game/System/DrawBuffer.cpp and its replay
# engine), J3DShape / J3DShapeMtx / J3DShapeDraw / J3DPacket and the J3D material path (as tools/test_par_dl.sh and tools/test_draw_pipe.sh build
# them); only the GX library is a stand-in that writes the production bytes. See the head of tests/draw_replay25.cpp.
# Passes when, for every seed, every FIFO size and every setting of the J3D list cuts and GXV_ARRAYSKIP:
#   - the bytes every frame put into the stream and the J3D / GX state after every drawer are the same for GXV_REPLAY25=1 (replay) and
#     GXV_REPLAY25=0 (today's draw); GXV_REPLAY25=2 (verify) draws for real and its census says 0 differed, 0 post-state wrong, and compared > 0;
#   - replays happen (the census counts them), and the kinds the engine leaves alone are drawn for real (counted);
#   - mutations of words the key holds (the shape's geometry list, its flags, CPU skinning, a user list) change nothing in any mode;
#   - a mutation of a word the key does NOT hold (a shape's matrix index: T_MUT=idx) makes the replay stale (mode 1 differs from mode 0:
#     the negative control) and verify mode finds it (differed > 0);
#   - AddressSanitizer + UBSan report nothing.
#   bash tools/test_replay25.sh [frames] [seeds]       (OUT: build directory, default a temporary one)
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
frames=${1:-90}
seeds=${2:-"1 2 3"}
out=${OUT:-$(mktemp -d)}
CXX=${CXX:-clang++}
CC=${CC:-clang}
units=(JSystem/J3DGraphBase/J3DShape JSystem/J3DGraphBase/J3DShapeMtx JSystem/J3DGraphBase/J3DShapeDraw JSystem/J3DGraphBase/J3DPacket
       JSystem/J3DGraphBase/J3DGD JSystem/J3DGraphBase/J3DTransform JSystem/J3DGraphBase/J3DStruct JSystem/J3DGraphBase/J3DMaterial
       JSystem/J3DGraphBase/J3DMatBlock JSystem/J3DGraphBase/J3DTevs JSystem/JMath/JMATrigonometric Game/System/DrawBuffer Game/LiveActor/ActorLightCtrl)
common=(-g -O1 -Wno-register -DTARGET_PC=1 -fno-strict-aliasing -w -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize=vptr,alignment
        -fno-sanitize-recover=undefined -include stdint.h -include "$root/tests/glibc_fabs_shim.h" -I"$root/platform/include"
        -I"$root/decomp/libs/JSystem/include" -I"$root/decomp/libs/RVL_SDK/include" -I"$root/decomp/libs/nw4r/include"
        -I"$root/decomp/libs/RVLFaceLib/include" -I"$root/decomp/include")
cxx=(-std=c++20 -fms-extensions -include cfloat -include "$root/platform/include/port/msl_ext.hpp")
objs=()
for u in "${units[@]}"; do
    o="$out/$(basename "$u").o"
    "$CXX" "${cxx[@]}" "${common[@]}" -c "$root/decomp/src/$u.cpp" -o "$o"
    objs+=("$o")
done
"$CC" -x c -Wno-implicit-function-declaration "${common[@]}" -c "$root/decomp/src/RVL_SDK/gd/GDBase.c" -o "$out/gdbase.o"
"$CC" -x c -Wno-implicit-function-declaration "${common[@]}" -c "$root/platform/src/sdk/mtx.c" -o "$out/mtx.o"
"$CXX" "${cxx[@]}" "${common[@]}" -c "$root/tests/draw_replay25.cpp" -o "$out/test.o"
RTLINK=""
if ! echo 'int main(){}' | "$CXX" -x c++ -fsanitize=address - -o "$out/rt-probe" 2>/dev/null; then
    RTLINK="-fno-sanitize-link-runtime -l:libasan.so.8 -l:libubsan.so.1"
fi
"$CXX" "${common[@]}" "$out/test.o" "${objs[@]}" "$out/gdbase.o" "$out/mtx.o" -pthread -lm $RTLINK -o "$out/draw_replay25"
export ASAN_OPTIONS=detect_leaks=0:verify_asan_link_order=0 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 GXV_REPLAY25_KB=8192
fail=0
runs=0
rm -f "$out/failed"
# run $1 label with environment, mode $2, seed $3: output in $out/m$2.txt, census in $out/m$2.err
run() {
    local mode=$1 seed=$2
    GXV_REPLAY25=$mode "$out/draw_replay25" "$frames" "$seed" > "$out/m$mode.txt" 2> "$out/m$mode.err" || { echo "FAIL: mode $mode seed $seed: $(tail -3 "$out/m$mode.err")"; fail=1; : > "$out/failed"; }
    runs=$((runs + 1))
}
census() { grep -m1 "^replay25: GXV_REPLAY25" "$out/m$1.err" | sed 's/replay25: //' | cut -c1-420; }
same() {  # $1 label: modes 1 and 2 against mode 0 for the seed in $2 (the environment is the caller's)
    local label=$1 seed=$2
    run 0 "$seed"; run 1 "$seed"; run 2 "$seed"
    for m in 1 2; do
        if ! cmp -s "$out/m0.txt" "$out/m$m.txt"; then
            echo "FAIL: $label seed $seed mode $m: the stream or the J3D state differs from mode 0's"; diff "$out/m0.txt" "$out/m$m.txt" | head -4; fail=1; : > "$out/failed"
        fi
    done
    if ! grep -q "verify:" "$out/m2.err"; then
        echo "$label seed $seed: replay is off in this configuration (no verify line); the streams are equal to mode 0's"; return
    fi
    if ! grep -q "verify: [0-9]* compared, 0 differed, 0 post-state wrong" "$out/m2.err"; then
        echo "FAIL: $label seed $seed: verify mode did not say 0 differed / 0 post-state wrong: $(grep -m1 'verify:' "$out/m2.err" | cut -c1-100)"; fail=1; : > "$out/failed"
    fi
    echo "$label seed $seed: $(census 1)"
}
for seed in $seeds; do
    for around in "T_FIFO=16384" "T_FIFO=1600" "T_FIFO=900 T_BATCH=1" "T_BATCH=1 T_ARRSKIP=1" "T_NOSTREAM=1" "GXV_J3D_DIFFSKIP=0" "GXV_J3D_MATMERGE=0 GXV_J3D_DIFFSKIP=0"; do
        ( export $around; same "$around" "$seed" )
    done
    for mut in list skin flags user; do
        ( export T_MUT=$mut; same "T_MUT=$mut (a word the key holds)" "$seed" )
    done
    # the negative control: a word the key does not hold. Mode 1 must be stale (differ from mode 0), verify must count it.
    export T_MUT=idx
    run 0 "$seed"; run 1 "$seed"; run 2 "$seed"
    if cmp -s "$out/m0.txt" "$out/m1.txt"; then echo "FAIL: negative control T_MUT=idx seed $seed: the stale replay was not seen in the stream"; fail=1; : > "$out/failed"
    else echo "negative control T_MUT=idx seed $seed: mode 1 is stale as it must be (the stream differs from mode 0's)"; fi
    if ! cmp -s "$out/m0.txt" "$out/m2.txt"; then echo "FAIL: verify mode changed the stream (seed $seed)"; fail=1; : > "$out/failed"; fi
    if grep -q "verify: [0-9]* compared, 0 differed" "$out/m2.err"; then echo "FAIL: negative control T_MUT=idx seed $seed: verify did not count the stale replay"; fail=1; : > "$out/failed"
    else echo "negative control T_MUT=idx seed $seed: verify mode counts it: $(grep -m1 -o 'verify: [0-9]* compared, [0-9]* differed, [0-9]* post-state wrong' "$out/m2.err")"; fi
    unset T_MUT
done
echo "$runs runs"
[ $fail = 0 ] && [ ! -e "$out/failed" ] && echo "test_replay25: PASS" || { echo "test_replay25: FAIL"; exit 1; }
