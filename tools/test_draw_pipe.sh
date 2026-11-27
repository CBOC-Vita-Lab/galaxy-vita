#!/usr/bin/env bash
# GXV_DRAWPIPE (drawpipe24, dpengine) on the host: tests/draw_pipe.cpp with the production draw buffers (Game/System/DrawBuffer.cpp,
# DrawBufferGroup.cpp, DrawBufferExecuter.cpp, DrawBufferHolder.cpp), Game/LiveActor/ActorLightCtrl.cpp, J3DPacket.cpp and the J3D
# material path it links (as tools/test_par_dl.sh builds it), port/draw_pipe.h and the engine of modes 3 / 4, port/draw_thread.h.
# Passes when, for every seed and every setting of GXV_J3D_DIFFSKIP / GXV_J3DNATIVE:
#   - the recorded GX stream of every turn, and what the draw leaves in j3dSys and the J3D statics, are the same for mode 0 (today's
#     draw), mode 1 (the frozen lists and the draw side's own J3D context, serial), mode 2 (the census / verify), mode 3 (the whole
#     draw on the draw thread while the game thread waits) and mode 4 (the draw recorded on the game thread and executed by the draw
#     thread beside the next update, joined before calc-anim);
#   - ThreadSanitizer and AddressSanitizer + UBSan report nothing, with the update writing every frozen field;
#   - the census sees exactly the writes the fixture makes in each interval, the verify finds no differing pass and no small field
#     changed by the draw;
#   - the scenarios hold: serial turns among pipelined ones, GX use by the logic (the early join), a recorder that abandons
#     mid-list or refuses, a list that moves in the draw, a draw thread that stalls (bounded join, watchdog, pipeline off) or
#     starts late (the job taken back);
#   - and the negative controls fail as they must.
#   bash tools/test_draw_pipe.sh [turns] [seeds]       (OUT: build directory, default a temporary one; nothing is deleted)
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
turns=${1:-120}
seeds=${2:-"1 2 3"}
out=${OUT:-$(mktemp -d)}
CXX=${CXX:-clang++}
CC=${CC:-clang}
units=(JSystem/J3DGraphBase/J3DMaterial JSystem/J3DGraphBase/J3DMatBlock JSystem/J3DGraphBase/J3DTevs JSystem/J3DGraphBase/J3DGD
       JSystem/J3DGraphBase/J3DPacket JSystem/J3DGraphBase/J3DTransform JSystem/J3DGraphBase/J3DStruct JSystem/JMath/JMATrigonometric
       Game/System/DrawBuffer Game/System/DrawBufferGroup Game/System/DrawBufferExecuter Game/System/DrawBufferHolder
       Game/LiveActor/ActorLightCtrl)
build() {  # $1: name, $2..: sanitizer flags
    local name=$1; shift
    local common=(-g -O1 -Wno-register -DTARGET_PC=1 -fno-strict-aliasing -w -fno-omit-frame-pointer "$@" -include stdint.h
                  -include "$root/tests/glibc_fabs_shim.h" -I"$root/platform/include" -I"$root/decomp/libs/JSystem/include"
                  -I"$root/decomp/libs/RVL_SDK/include" -I"$root/decomp/libs/nw4r/include" -I"$root/decomp/libs/RVLFaceLib/include"
                  -I"$root/decomp/include")
    # (std::mem_func / for_each_array: the game's library extensions, which the console build gets through port/compat.h)
    local cxx=(-std=c++20 -fms-extensions -include cfloat -include "$root/platform/include/port/msl_ext.hpp")
    local objs=()
    for u in "${units[@]}"; do
        local o="$out/$name-$(basename "$u").o"
        "$CXX" "${cxx[@]}" "${common[@]}" -c "$root/decomp/src/$u.cpp" -o "$o"
        objs+=("$o")
    done
    "$CC" -x c -Wno-implicit-function-declaration "${common[@]}" -c "$root/decomp/src/RVL_SDK/gd/GDBase.c" -o "$out/$name-gdbase.o"
    "$CC" -x c -Wno-implicit-function-declaration "${common[@]}" -c "$root/platform/src/sdk/mtx.c" -o "$out/$name-mtx.o"
    "$CXX" "${cxx[@]}" "${common[@]}" -c "$root/tests/draw_pipe.cpp" -o "$out/$name-test.o"
    "$CXX" "${common[@]}" "$out/$name-test.o" "${objs[@]}" "$out/$name-gdbase.o" "$out/$name-mtx.o" \
        -pthread -lm ${RTLINK[$name]} -o "$out/draw_pipe-$name"
    if [ "$name" = tsan ]; then
        # the negative control DP_NEG=guess: the same build with the draw buffers' holder compiled without the token's guess
        "$CXX" "${cxx[@]}" "${common[@]}" -DPORT_TEST_NO_GUESS=1 -c "$root/decomp/src/Game/System/DrawBufferHolder.cpp" -o "$out/noguess-DrawBufferHolder.o"
        local nobjs=()
        for o in "${objs[@]}"; do
            case $o in */tsan-DrawBufferHolder.o) nobjs+=("$out/noguess-DrawBufferHolder.o");; *) nobjs+=("$o");; esac
        done
        "$CXX" "${common[@]}" "$out/$name-test.o" "${nobjs[@]}" "$out/$name-gdbase.o" "$out/$name-mtx.o" \
            -pthread -lm ${RTLINK[$name]} -o "$out/draw_pipe-noguess"
    fi
}
# (the sanitizer runtimes: clang's when installed, else GCC's, as tools/test_par_dl.sh links them)
declare -A RTLINK=([tsan]="" [asan]="")
if ! echo 'int main(){}' | "$CXX" -x c++ -fsanitize=thread - -o "$out/rt-probe" 2>/dev/null; then
    RTLINK=([tsan]="-fno-sanitize-link-runtime -l:libtsan.so.2" [asan]="-fno-sanitize-link-runtime -l:libasan.so.8 -l:libubsan.so.1")
fi
build tsan -fsanitize=thread
build asan -fsanitize=address,undefined -fno-sanitize=vptr,alignment -fno-sanitize-recover=undefined
export TSAN_OPTIONS="halt_on_error=1 exitcode=66" ASAN_OPTIONS=detect_leaks=0:verify_asan_link_order=0 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
fail=0
runs=0
# one run against mode 0's output: $1 label, $2 binary, $3 mode, $4 seed (the environment is the caller's; mode 0 runs with it too)
same() {
    local label=$1 b=$2 mode=$3 seed=$4
    "$out/draw_pipe-$b" 0 "$turns" "$seed" > "$out/m0.txt" 2> "$out/m0.err" || { echo "FAIL: $b mode 0 seed $seed ($label)"; tail -5 "$out/m0.err"; fail=1; }
    runs=$((runs + 1))
    if ! "$out/draw_pipe-$b" "$mode" "$turns" "$seed" > "$out/m.txt" 2> "$out/m.err"; then
        echo "FAIL: $b mode $mode seed $seed ($label)"; tail -12 "$out/m.err"; fail=1
    elif ! cmp -s "$out/m0.txt" "$out/m.txt"; then
        echo "FAIL: $b mode $mode seed $seed ($label): the GX stream (or what the draw left in j3dSys) differs from mode 0's"
        diff "$out/m0.txt" "$out/m.txt" | head -4 || true; fail=1
    else
        echo "$b $label: $(grep -m1 '^mode' "$out/m.err" | cut -c1-200)"
        if [ "$mode" -ge 3 ]; then echo "    $(grep -m1 '^engine' "$out/m.err")"; fi
    fi
}
for seed in $seeds; do
    for skip in 1 0; do
        for native in 0 1; do
            export DP_DIFFSKIP=$skip DP_NATIVE=$native
            for b in tsan asan; do
                for mode in 1 2 3 4; do
                    same "DIFFSKIP=$skip J3DNATIVE=$native" $b $mode $seed
                done
            done
        done
    done
done
export DP_DIFFSKIP=1 DP_NATIVE=1
# the scenarios of modes 3 / 4 (each run checks by itself that its scenario happened: "NOT ENGAGED" fails it)
for b in tsan asan; do
    for mode in 3 4; do
        DP_SERIAL=3 same "a serial turn every third" $b $mode 1
        DP_LOGICGX=1 same "the logic uses GX" $b $mode 2
    done
    DP_LOGICGX=1 DP_SERIAL=4 same "the logic uses GX, serial turns between" $b 4 3
    # (a capture of this fixture is about 95 calls: the abandon point swept over it, so that it falls before the first pass, between
    # two passes, in the middle of a model the game thread draws by hand, and inside each of the two fur draws)
    for at in $( [ $b = asan ] && echo "2 6 10 14 18 22 26 30 34 38 42 46 50 54 58 62 66 70 74 78 82 86 90 94" || echo "3 16 29 42 55 59 68 81 94" ); do
        DP_ABANDON=$at same "the recorder abandons at its call $at, every third turn" $b 4 1
    done
    DP_ABANDON=58 DP_LOGICGX=1 DP_SERIAL=5 same "abandons, GX use by the logic and serial turns together" $b 4 2
    DP_ABANDON=12 DP_NOOPEN=1 same "abandons at call 12; the tokens after it executed by the recorder's handler" $b 4 3
    DP_BUSY=1 same "the recorder refuses every fourth capture" $b 4 1
    DP_UPLATE=5000 same "the draw thread reports in 5 ms late: still used (mode 3)" $b 3 1
    DP_UPLATE=5000 same "the draw thread reports in 5 ms late: jobs taken back until it runs (mode 4)" $b 4 2
    DP_NESTED=1 same "an interrupt handler uses GX at the join's safe point (a join inside the join)" $b 4 1
    DP_NESTED=1 DP_LOGICGX=1 DP_ABANDON=20 same "the same with GX use by the logic and abandons" $b 4 3
    DP_OTHER=1 same "another thread waits for the draw thread's job every third turn" $b 4 2
    DP_OTHER=1 DP_STALL=4 DP_STALL_MS=60 DP_WATCHDOG_MS=20 same "the same with the draw thread stalling: the other thread gives up at the watchdog" $b 4 3
    DP_STALL=5 DP_STALL_MS=60 DP_WATCHDOG_MS=20 same "the draw thread stalls in turn 5 (watchdog 20 ms)" $b 4 1
    DP_LATE=4000 same "the draw thread starts 4 ms late: jobs taken back" $b 4 1
    DP_LATE=4000 same "the same in mode 3: the game thread takes the job back after 2 ms and draws" $b 3 1
    DP_LATE=4000 DP_MAXTAKEN=6 same "the same, the pipeline off after 6 in a row" $b 4 2
    DP_NEG=midlist same "a list moves in the draw (mode 1: epoch fall-back)" $b 1 1
    DP_NEG=midlist same "a list moves in the draw (mode 2: counted)" $b 2 1
    DP_NEG=midlist same "a list moves in the draw (mode 4: the recording ended there)" $b 4 1
    DP_NEG=midsmall same "the draw changes the camera and the pointer (mode 2: counted)" $b 2 1
done
# Mode 0 of the patched draw buffers against the base revisions' (the files the two rounds rewrote, taken from git into the build
# directory): b7546d4 is the tree before GXV_DRAWPIPE, 9d80b80 the first round. The rewrites changed nothing in mode 0.
for baserev in ${DP_BASE_REVS:-b7546d4 9d80b80}; do
    if git -C "$root" cat-file -e "$baserev^{commit}" 2>/dev/null; then
        bdir="$out/base-$baserev"
        mkdir -p "$bdir/src" "$bdir/inc/Game/System"
        for f in DrawBuffer DrawBufferGroup DrawBufferExecuter DrawBufferHolder; do
            git -C "$root" show "$baserev:decomp/src/Game/System/$f.cpp" > "$bdir/src/$f.cpp"
            git -C "$root" show "$baserev:decomp/include/Game/System/$f.hpp" > "$bdir/inc/Game/System/$f.hpp"
        done
        # (the base files name the draw's J3DSys j3dSys; the J3D units linked with them are this tree's: the same object in mode 0)
        bflags=(-g -O1 -Wno-register -DTARGET_PC=1 -DDP_BASE=1 -fno-strict-aliasing -w -fno-omit-frame-pointer -fsanitize=address,undefined
                -fno-sanitize=vptr,alignment -fno-sanitize-recover=undefined -include stdint.h -include "$root/tests/glibc_fabs_shim.h"
                -I"$bdir/inc" -I"$root/platform/include" -I"$root/decomp/libs/JSystem/include" -I"$root/decomp/libs/RVL_SDK/include"
                -I"$root/decomp/libs/nw4r/include" -I"$root/decomp/libs/RVLFaceLib/include" -I"$root/decomp/include"
                -std=c++20 -fms-extensions -include cfloat -include "$root/platform/include/port/msl_ext.hpp")
        bobjs=()
        for f in DrawBuffer DrawBufferGroup DrawBufferExecuter DrawBufferHolder; do
            "$CXX" "${bflags[@]}" -c "$bdir/src/$f.cpp" -o "$bdir/$f.o"; bobjs+=("$bdir/$f.o")
        done
        for u in "${units[@]}"; do  # (the units the patch did not touch, and J3DPacket: the AddressSanitizer build's objects)
            case $u in Game/System/*) ;; *) bobjs+=("$out/asan-$(basename "$u").o");; esac
        done
        "$CXX" "${bflags[@]}" "$root/tests/draw_pipe.cpp" "${bobjs[@]}" "$out/asan-gdbase.o" "$out/asan-mtx.o" -pthread -lm ${RTLINK[asan]} -o "$bdir/draw_pipe-base"
        for seed in $seeds; do
            for cfg in "1 0 0" "0 0 0" "1 1 0" "1 0 1"; do
                set -- $cfg
                export DP_DIFFSKIP=$1 DP_NATIVE=$2 DP_MATMERGE=$3
                "$bdir/draw_pipe-base" 0 "$turns" "$seed" > "$out/b0.txt" 2> "$out/b0.err" || { echo "FAIL: base build $baserev, seed $seed"; tail -5 "$out/b0.err"; fail=1; }
                "$out/draw_pipe-asan" 0 "$turns" "$seed" > "$out/m0.txt" 2> /dev/null
                runs=$((runs + 1))
                if cmp -s "$out/b0.txt" "$out/m0.txt"; then
                    echo "mode 0 = base $baserev (seed $seed, DIFFSKIP=$1 J3DNATIVE=$2 MATMERGE=$3): $(grep -m1 -o 'stream [0-9a-f]*' "$out/b0.err")"
                else
                    echo "FAIL: mode 0 differs from the base revision $baserev's draw (seed $seed, DIFFSKIP=$1 J3DNATIVE=$2 MATMERGE=$3)"; fail=1
                fi
            done
        done
        unset DP_MATMERGE
    else
        echo "NOTE: revision $baserev is not in this checkout: mode 0 was not compared with its draw buffers"
    fi
done
export DP_DIFFSKIP=1 DP_NATIVE=0
# GXV_J3D_MATMERGE=1 (the console's setting): the merged list's block comes from the drawer's guest heap, which the fixture does not
# have, so every drawer declines to merge; the path up to the allocation (in mode 4: up to the note for the game thread) is drawn
for b in tsan asan; do
    for mode in 1 2 3 4; do
        DP_MATMERGE=1 same "MATMERGE=1" $b $mode 1
    done
done
"$out/draw_pipe-asan" 2 "$turns" 1 2>&1 >/dev/null | grep "^drawpipe:" | cut -c1-400 | sed 's/^/  /'
"$out/draw_pipe-asan" 4 "$turns" 1 2>&1 >/dev/null | grep "^drawpipe:" | cut -c1-600 | sed 's/^/  /'
# negative controls (mode 4): each must fail (a ThreadSanitizer report, exit 66, or an output that differs from mode 0's)
export DP_DIFFSKIP=1 DP_NATIVE=1
"$out/draw_pipe-tsan" 0 "$turns" 1 > "$out/m0.txt" 2> /dev/null
# (capall: the draw's begin and end recorded with the middle, as one serial draw; endearly: the end made at the hand-over;
# keep: every token claims that its pass draws nothing; fur: the fur draw does not hand its shape's words to the pipeline)
for neg in live open sys light cam ext capall endearly keep fur; do
    if DP_NEG=$neg "$out/draw_pipe-tsan" 4 "$turns" 1 > "$out/neg.txt" 2> "$out/neg.err" && cmp -s "$out/m0.txt" "$out/neg.txt"; then
        echo "FAIL: negative control DP_NEG=$neg passed"; fail=1
    else
        why=$(grep -m1 -o "WARNING: ThreadSanitizer: [a-z -]*" "$out/neg.err" || true)
        [ -n "$why" ] || why=$(grep -m1 "^NOT ENGAGED\|^WRONG CLAIM" "$out/neg.err" | cut -c1-120 || true)
        echo "negative control DP_NEG=$neg (mode 4): fails as it must (${why:-the stream differs})"
    fi
done
# (a recording ended inside a fur draw, and the fur draw has not said that the packet is pointed at a layer model)
if DP_NEG=furlent DP_ABANDON=58 "$out/draw_pipe-tsan" 4 "$turns" 1 > "$out/neg.txt" 2> "$out/neg.err" && cmp -s "$out/m0.txt" "$out/neg.txt"; then
    echo "FAIL: negative control DP_NEG=furlent passed"; fail=1
else
    echo "negative control DP_NEG=furlent with DP_ABANDON=58 (mode 4): fails as it must (the stream differs)"
fi
# the token's guess of the J3D statics a pass leaves: without it (a build of the holder that does not make it) the model drawn by
# hand after a pass sends another stream, in the serial rehearsal and across the threads alike, and the misses are counted
for mode in 1 4; do
    if DP_NEG=guess "$out/draw_pipe-noguess" $mode "$turns" 1 > "$out/neg.txt" 2> "$out/neg.err" && ! cmp -s "$out/m0.txt" "$out/neg.txt"; then
        echo "negative control DP_NEG=guess (mode $mode): fails as it must (the stream differs; $(grep -m1 -o 'statics guessed wrong [0-9]*' "$out/neg.err"))"
    else
        echo "FAIL: negative control DP_NEG=guess, mode $mode"; tail -3 "$out/neg.err"; fail=1
    fi
done
echo "$runs runs against mode 0"
[ $fail = 0 ] && echo "test_draw_pipe: PASS" || { echo "test_draw_pipe: FAIL"; exit 1; }
