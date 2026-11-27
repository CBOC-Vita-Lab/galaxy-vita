#!/usr/bin/env bash
# GXV_BOWL_POSE host proof (bowl31): tests/bowl_pose.cpp, the fish pose rule (port/bowl_pose.h, LiveActor.cpp PortBowlPose and the flush) against the game's
# own XanimeCore / J3DJoint / J3DJointTree / J3DAnimation / J3DMtxBuffer code (built as tools/test_par_anim_walk.sh builds them), two worlds byte for byte
# over five turn rhythms; then the mutants, each of which must fail. Run with: bash tools/test_bowl_pose.sh [turns] [seed]
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=$(mktemp -d "${TMPDIR:-/tmp}/bowl-pose.XXXXXX")
trap 'rm -rf "$out"' EXIT
D=$root/decomp
turns=${1:-1500}
seed=${2:-4242}
GAMECXX=${GAMECXX:-clang++}
CXX=${CXX:-g++}
flags=(-std=gnu++17 -O1 -g -DTARGET_PC=1 -DNDEBUG=1 -DVERSION=0 -fshort-wchar -fsigned-char -fno-strict-aliasing -fwrapv -fms-extensions
       -fno-exceptions -fno-rtti -ffp-contract=on -w -Wno-register -include string -include "$root/platform/include/port/compat.h"
       -I"$root/platform/include" -I"$D/include" -I"$D/libs/JSystem/include" -I"$D/libs/RVL_SDK/include" -I"$D/libs/nw4r/include" -I"$root/tests")
units=(Game/Animation/XanimeCore JSystem/J3DGraphAnimator/J3DJoint JSystem/J3DGraphAnimator/J3DJointTree JSystem/J3DGraphAnimator/J3DAnimation
       JSystem/J3DGraphAnimator/J3DMtxBuffer JSystem/J3DGraphBase/J3DTransform JSystem/JMath/JMath JSystem/JMath/JMATrigonometric
       Game/Util/MathUtil Game/Util/MtxUtil Game/Util/JointController)
objs=()
for f in "${units[@]}"; do
    "$GAMECXX" "${flags[@]}" -c "$D/src/$f.cpp" -o "$out/${f##*/}.o"
    objs+=("$out/${f##*/}.o")
done
"$GAMECXX" "${flags[@]}" -DPAR_ANIM_REAL_MATHUTIL -DPAR_ANIM_REAL_MTXUTIL -c "$root/tests/par_anim_walk_stubs.cpp" -o "$out/stubs.o"
"${CC:-gcc}" -O1 -I"$root/platform/include" -I"$D/libs/RVL_SDK/include" -include stdint.h -include "$root/tests/glibc_fabs_shim.h" -w \
    -c "$root/platform/src/sdk/mtx.c" -o "$out/mtx.o"
"$GAMECXX" "${flags[@]}" -c "$root/tests/bowl_pose.cpp" -o "$out/test.o"
"$CXX" -pthread "${objs[@]}" "$out/stubs.o" "$out/mtx.o" "$out/test.o" -Wl,--unresolved-symbols=ignore-all -o "$out/run"
fail=0
for sc in fish change-on-drawn smooth reader change-any; do
    "$out/run" "$turns" "$seed" "$sc" 0 || fail=1
    "$out/run" "$turns" $((seed + 1)) "$sc" 0 > /dev/null || { echo "second seed of $sc failed"; fail=1; }
done
# Mutants: each must fail somewhere (scenario where it shows).
for m in "1 fish" "2 fish" "3 fish" "4 fish" "5 smooth" "6 reader"; do
    set -- $m
    if "$out/run" "$turns" "$seed" "$2" "$1" > "$out/mut$1.log" 2>&1; then
        echo "mutant $1 ($2) PASSED: the test cannot see that fault"; tail -7 "$out/mut$1.log"; fail=1
    else
        echo "== mutant $1 ($2): fails, as it must ($(grep -m1 -- '<-- FAIL' "$out/mut$1.log" | sed 's/  */ /g' | cut -c1-150))"
    fi
done
[ $fail -eq 0 ] || { echo "bowl_pose: FAIL"; exit 1; }
echo "bowl_pose: PASS"
