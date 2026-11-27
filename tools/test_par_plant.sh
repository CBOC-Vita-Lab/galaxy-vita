#!/usr/bin/env bash
# GXV_PAR_PLANT host proof (tests/par_plant.cpp): the plant jobs' protocol (port/par_plant.h phase byte + port/par_spec.h worker) under
# ThreadSanitizer and AddressSanitizer/UBSan; then the negative control (no PlantGameOwn), which ThreadSanitizer must reject.
#   bash tools/test_par_plant.sh [pictures] [plants]      (BASE_ROOT=<full tree> when this tree is sparse)
# parplant23: also the game's turn order ("turns" mode: GXV_PAR_PLANT_EARLY, port/par_plant.h PlantTurns) and its negative control.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
base=${BASE_ROOT:-$root}
out=${OUT:-$(mktemp -d)}
[ -z "${OUT:-}" ] && trap 'rm -rf "$out"' EXIT
pictures=${1:-300}
plants=${2:-200}
inc=(-I"$root/platform/include" -I"$base/platform/include")
CXX=${CXX:-g++}
nice -n 10 "$CXX" -std=c++17 -O1 -g -fsanitize=thread "${inc[@]}" "$root/tests/par_plant.cpp" -pthread -o "$out/par_plant_tsan"
nice -n 10 "$CXX" -std=c++17 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer "${inc[@]}" "$root/tests/par_plant.cpp" -pthread \
    -o "$out/par_plant_asan"
fail=0
for seed in 1 2 3; do
    TSAN_OPTIONS="halt_on_error=1 exitcode=66" "$out/par_plant_tsan" "$pictures" "$plants" "$seed" || fail=1
    ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 "$out/par_plant_asan" "$pictures" "$plants" "$seed" || fail=1
done
# parplant23: the game's turn order (drawn / skipped turns, GXV_PAR_PLANT_EARLY's prediction, the early batch ended before update()):
# early mode 1 must give the worker most of the plants (>= 50 %) with 0 mismatches; mode 0 (2.3 round 2's place) and 2 are reported
for seed in 1 2 3; do
    TSAN_OPTIONS="halt_on_error=1 exitcode=66" "$out/par_plant_tsan" 300 "$plants" "$seed" turns 1 2000 || fail=1
    ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 "$out/par_plant_asan" 300 "$plants" "$seed" turns 1 2000 || fail=1
done
TSAN_OPTIONS="halt_on_error=1 exitcode=66" "$out/par_plant_tsan" 300 "$plants" 4 turns 0 2000 || fail=1
TSAN_OPTIONS="halt_on_error=1 exitcode=66" "$out/par_plant_tsan" 300 "$plants" 4 turns 2 2000 || fail=1
# negative control of the turn order: the early batch left open into update(): the worker's reads race update()'s writes
set +e
TSAN_OPTIONS="halt_on_error=1 exitcode=66" "$out/par_plant_tsan" 100 "$plants" 7 turns-neg 1 2000 > "$out/neg_turns.log" 2>&1
rc=$?
set -e
if [ "$rc" = 66 ] && grep -q "data race" "$out/neg_turns.log"; then
    echo "negative control (turns): ThreadSanitizer reports the race (as it must)"
else
    echo "FAIL negative control (turns): exit $rc, no race reported"; fail=1
fi
# negative control: without the game thread's PlantGameOwn the worker's reads race the flush's writes
set +e
TSAN_OPTIONS="halt_on_error=1 exitcode=66" "$out/par_plant_tsan" 40 "$plants" 7 neg > "$out/neg.log" 2>&1
rc=$?
set -e
if [ "$rc" = 66 ] && grep -q "data race" "$out/neg.log"; then
    echo "negative control: ThreadSanitizer reports the race (as it must)"
else
    echo "FAIL negative control: exit $rc, no race reported"; fail=1
fi
[ $fail = 0 ] && echo "test_par_plant: PASS" || echo "test_par_plant: FAIL"
exit $fail
