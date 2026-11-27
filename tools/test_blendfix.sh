#!/usr/bin/env bash
# GXV_POSE_BLENDFIX host proof (blendfix31): tests/blendfix.cpp, the blend start after a left-out pose for every pose cut (pair / phase / bowl / lean /
# every2 / every4), against the game's own XanimeCore / J3DJoint / J3DJointTree / J3DAnimation / J3DMtxBuffer code (built as tools/test_bowl_pose.sh builds
# them), two worlds byte for byte over five turn rhythms. 1) the fix on: every computation, picture and logic state equal, three actor kinds, two seeds;
# 2) the mutants, each of which must fail; 3) the fix off: the measurement (how often a blend starts stale, how far off), which must show the hole;
# 4) the cost line. Run with: bash tools/test_blendfix.sh [turns] [seed]
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=$(mktemp -d "${TMPDIR:-/tmp}/blendfix.XXXXXX")
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
"$GAMECXX" "${flags[@]}" -c "$root/tests/blendfix.cpp" -o "$out/test.o"
"$CXX" -pthread "${objs[@]}" "$out/stubs.o" "$out/mtx.o" "$out/test.o" -Wl,--unresolved-symbols=ignore-all -o "$out/run"
if [ -n "${BLENDFIX_BUILD_ONLY:-}" ]; then cp "$out/run" "$BLENDFIX_BUILD_ONLY"; exit 0; fi  # (a binary to run by hand)
fail=0
# 1) the fix on: everything equal
for kind in real smooth stress; do
    "$out/run" "$turns" "$seed" "$kind" all 1 0 40 | grep -v '^blendfix' || true
    "$out/run" "$turns" "$seed" "$kind" all 1 0 40 > /dev/null || { echo "fix on, $kind: FAIL"; fail=1; }
    "$out/run" "$turns" $((seed + 1)) "$kind" all 1 0 120 > /dev/null || { echo "fix on, $kind, second seed, frequent changes: FAIL"; fail=1; }
done
# 2) mutants (fix on): each must fail where it shows
for m in "1 real pair" "1 real lean" "2 real every4" "3 smooth lean" "4 smooth pair" "5 smooth every4" "6 real pair"; do
    set -- $m
    if "$out/run" "$turns" "$seed" "$2" "$3" 1 "$1" 40 > "$out/mut$1.log" 2>&1; then
        echo "mutant $1 ($2 $3) PASSED: the test cannot see that fault"; tail -6 "$out/mut$1.log"; fail=1
    else
        echo "== mutant $1 ($2 $3): fails, as it must ($(grep -m1 -- '<-- FAIL' "$out/mut$1.log" | sed 's/  */ /g' | cut -c1-170))"
    fi
done
# 3) the fix off: the measurement; the hole must show for the every-n-th rules and the pair rule
echo "== measurement, the fix off (as integ-3.1 before blendfix31): 4 changes per 100 turns per actor"
{ "$out/run" "$turns" "$seed" real all 0 0 40 || true; } | grep -v '^blendfix' | tee "$out/old.log"
for r in pair every2 every4; do
    grep -E "^real +$r +pair " "$out/old.log" | grep -qE "blends off [1-9]" || { echo "measurement: no stale blend shown for $r (the harness lost the hole)"; fail=1; }
done
echo "== measurement, rarer and more frequent changes (1 and 12 per 100 turns per actor), pair rhythm"
for pm in 10 120; do
    { "$out/run" "$turns" "$seed" real all 0 0 $pm || true; } | grep -E "^real +[a-z0-9]+ +pair " || true
done
"$out/run" 0 0 cost
[ $fail -eq 0 ] || { echo "blendfix: FAIL"; exit 1; }
echo "blendfix: PASS"
