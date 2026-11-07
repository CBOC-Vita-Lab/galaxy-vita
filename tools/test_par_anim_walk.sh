#!/usr/bin/env bash
# GXV_PAR_ANIM host proof (joint15): tests/par_anim_walk.cpp. The speculative joint walk (decomp/include/Game/Animation/
# XanimeSpecWalk.hpp) on the worker (platform/include/port/par_spec.h) against the game's own XanimeCore / J3DJoint / J3DJointTree /
# J3DAnimation / J3DMtxBuffer code, byte for byte, under ThreadSanitizer; the worker's host branch and its console branch (over the
# kernel stand-ins of tests/par_stubs); then the negative controls, each of which must fail.
# The game units and the test need the game's dialect (clang++, as the game build; clang's own TSan runtime is not needed: the
# objects are linked by g++ with its libtsan). Run with: bash tools/test_par_anim_walk.sh [rounds] [seed]
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=$(mktemp -d "${TMPDIR:-/tmp}/par-anim.XXXXXX")
trap 'rm -rf "$out"' EXIT
D=$root/decomp
rounds=${1:-300}
seed=${2:-12345}
GAMECXX=${GAMECXX:-clang++}
CXX=${CXX:-g++}
flags=(-std=gnu++17 -O1 -g -DTARGET_PC=1 -DNDEBUG=1 -DVERSION=0 -fshort-wchar -fsigned-char -fno-strict-aliasing -fwrapv -fms-extensions
       -fno-exceptions -fno-rtti -ffp-contract=on -w -Wno-register -include string -include "$root/platform/include/port/compat.h"
       -I"$root/platform/include" -I"$D/include" -I"$D/libs/JSystem/include" -I"$D/libs/RVL_SDK/include" -I"$D/libs/nw4r/include")
units=(Game/Animation/XanimeCore JSystem/J3DGraphAnimator/J3DJoint JSystem/J3DGraphAnimator/J3DJointTree JSystem/J3DGraphAnimator/J3DAnimation
       JSystem/J3DGraphAnimator/J3DMtxBuffer JSystem/J3DGraphBase/J3DTransform JSystem/JMath/JMath JSystem/JMath/JMATrigonometric
       Game/Util/MathUtil Game/Util/MtxUtil Game/Util/JointController)  # (MathUtil: MR::vecBlend, calcSingle's interpolation:
       # the "blend" runs, GXV_PAR_ANIM_BLEND; MtxUtil and JointController: the "cb" runs, GXV_PAR_ANIM_CB)
build() {  # build <name> <extra flags...>: the game units, the stubs, mtx.c and the test, with the given flags
    local name=$1
    shift
    local objs=()
    mkdir -p "$out/$name"
    for f in "${units[@]}"; do
        "$GAMECXX" "${flags[@]}" -fsanitize=thread "$@" -c "$D/src/$f.cpp" -o "$out/$name/${f##*/}.o"
        objs+=("$out/$name/${f##*/}.o")
    done
    "$GAMECXX" "${flags[@]}" -fsanitize=thread -DPAR_ANIM_REAL_MATHUTIL -DPAR_ANIM_REAL_MTXUTIL "$@" -c "$root/tests/par_anim_walk_stubs.cpp" -o "$out/$name/stubs.o"
    "${CC:-gcc}" -O1 -fsanitize=thread -I"$root/platform/include" -I"$D/libs/RVL_SDK/include" -include stdint.h \
        -include "$root/tests/glibc_fabs_shim.h" -w -c "$root/platform/src/sdk/mtx.c" -o "$out/$name/mtx.o"
    "$GAMECXX" "${flags[@]}" -fsanitize=thread "$@" -c "$root/tests/par_anim_walk.cpp" -o "$out/$name/test.o"
    objs+=("$out/$name/stubs.o" "$out/$name/mtx.o" "$out/$name/test.o")
    # (what the game units reference on paths the test never runs: materials, draw buffers, GX)
    "$CXX" -fsanitize=thread -pthread "${objs[@]}" -Wl,--unresolved-symbols=ignore-all -o "$out/$name/run"
}
build host
echo "== host branch of the worker, -fsanitize=thread"
TSAN_OPTIONS=halt_on_error=1 "$out/host/run" "$rounds" "$seed"
TSAN_OPTIONS=halt_on_error=1 "$out/host/run" "$rounds" $((seed + 1))
echo "== GXV_PAR_ANIM_BLEND (blend states through Input::prev), and GXV_PAR_FLUSH's order (the worker from the end)"
TSAN_OPTIONS=halt_on_error=1 "$out/host/run" "$rounds" "$seed" blend
TSAN_OPTIONS=halt_on_error=1 "$out/host/run" "$rounds" $((seed + 2)) reverse
TSAN_OPTIONS=halt_on_error=1 "$out/host/run" "$rounds" $((seed + 3)) blend reverse
echo "== GXV_PAR_ANIM_CB (the NPC scale joint callback in the walk)"
TSAN_OPTIONS=halt_on_error=1 "$out/host/run" "$rounds" $((seed + 4)) cb
TSAN_OPTIONS=halt_on_error=1 "$out/host/run" "$rounds" $((seed + 5)) cb blend reverse
# (the console branch: the kernel calls of par_spec.h over tests/par_stubs, as tools/test_par_exec_view.sh does for par_exec.h;
# only the worker's header sees __vita__ there: the game units are the host build's)
mkdir -p "$out/vita"
"$GAMECXX" "${flags[@]}" -fsanitize=thread -D__vita__ -I"$root/tests/par_stubs" -c "$root/tests/par_anim_walk.cpp" -o "$out/vita/test.o"
"$CXX" -std=c++17 -O1 -fsanitize=thread -c "$root/tests/par_exec_view_stubs.cpp" -o "$out/vita/kernel.o"
vobjs=()
for f in "${units[@]}"; do vobjs+=("$out/host/${f##*/}.o"); done
"$CXX" -fsanitize=thread -pthread "${vobjs[@]}" "$out/host/stubs.o" "$out/host/mtx.o" "$out/vita/test.o" "$out/vita/kernel.o" \
    -Wl,--unresolved-symbols=ignore-all -o "$out/vita/run"
echo "== console branch of the worker (kernel stand-ins), -fsanitize=thread"
TSAN_OPTIONS=halt_on_error=1 "$out/vita/run" "$rounds" "$seed"
# Negative controls: each must fail (the test can see a wrong frame, a wrong base matrix, a missing J3DSys::mParentS, a result read
# without the claim).
fail=0
for neg in NEG_FRAME NEG_BASE NEG_PARENTS NEG_NOCLAIM NEG_PREV NEG_CB; do
    mode=; [ $neg = NEG_PREV ] && mode=blend  # (the copy of the core's joint state exists only for blend-state jobs)
    [ $neg = NEG_CB ] && mode=cb
    mkdir -p "$out/$neg"
    "$GAMECXX" "${flags[@]}" -fsanitize=thread -D$neg -c "$root/tests/par_anim_walk.cpp" -o "$out/$neg/test.o"
    "$CXX" -fsanitize=thread -pthread "${vobjs[@]}" "$out/host/stubs.o" "$out/host/mtx.o" "$out/$neg/test.o" \
        -Wl,--unresolved-symbols=ignore-all -o "$out/$neg/run"
    if TSAN_OPTIONS="halt_on_error=1 report_signal_unsafe=0" "$out/$neg/run" 60 "$seed" $mode > "$out/$neg/log" 2>&1; then
        echo "negative control $neg PASSED: the test cannot see that fault"
        tail -3 "$out/$neg/log"
        fail=1
    else
        echo "== negative control $neg: fails, as it must ($(grep -m1 -oE 'mismatches: .*|ThreadSanitizer: data race' "$out/$neg/log"))"
    fi
done
[ $fail -eq 0 ] || exit 1
echo "par_anim_walk: PASS"
