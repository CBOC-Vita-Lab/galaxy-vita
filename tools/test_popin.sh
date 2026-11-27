#!/bin/bash
# popin24: host test of platform/include/port/popin.h (tests/popin.cpp). Needs a C++17 compiler. Then the zoomclip23 test (the real
# ClippingJudge.cpp) is run as a regression: GXV_POPIN_CLIPPAD unset must leave it passing, and with GXV_POPIN_CLIPPAD=10 the logic frustum is
# only wider, so no sphere of the original picture may be clipped.
set -uo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
out="${TMPDIR:-/tmp}/popin_test.$$"
mkdir -p "$out"
CXX="${CXX:-clang++}"
"$CXX" -std=gnu++17 -O1 -Wall -I"$root/platform/include" "$root/tests/popin.cpp" -o "$out/popin" || { echo "build FAILED"; exit 2; }
rc=0
"$out/popin" || rc=1
# popin31b: the per-kind knobs as the game reads them (each case its own process: the knobs are read once)
env -u GXV_PB_MINPX_SHADOW -u GXV_PB_MINPX_DECOR -u GXV_PB_MINPX_FUR -u GXV_PB_MINPX_LOD -u GXV_PB_MINPX_SPECK -u GXV_PB_MINPX_STARBIT -u GXV_PB_MINPX_PARTICLE \
    GXV_PB_MINPX=3 GXV_PB_MINPX_SHADOW=0 GXV_PB_MINPX_FUR=6 "$out/popin" env-a || rc=1
env -u GXV_PB_MINPX GXV_PB_MINPX_DECOR=2 "$out/popin" env-b || rc=1
GXV_PB_MINPX=4 "$out/popin" env-c || rc=1
# popin31b: the mutants (platform/include/port/popin.h POPIN_MUTANT), each of which must fail: 1 an unset kind does not inherit GXV_PB_MINPX, 2 the plant reveal
# floored as a shadow, 3 the release forgotten by the kind guards, 4 every kind takes the first kind's constants, 5 the census ignores the kinds' floors,
# 6 the price line leaves the kept count out, 7 the band the wrong way round
for m in 1 2 3 4 5 6 7; do
    if ! "$CXX" -std=gnu++17 -O1 -Wall -DPOPIN_MUTANT=$m -I"$root/platform/include" "$root/tests/popin.cpp" -o "$out/popin-m$m" 2> "$out/m$m.err"; then
        echo "mutant $m: build FAILED"; cat "$out/m$m.err"; rc=1; continue
    fi
    if "$out/popin-m$m" > "$out/m$m.log" 2>&1 && GXV_PB_MINPX=3 GXV_PB_MINPX_SHADOW=0 GXV_PB_MINPX_FUR=6 "$out/popin-m$m" env-a > "$out/m$m-env.log" 2>&1; then
        echo "mutant $m PASSED: the test cannot see that fault"; rc=1
    else
        echo "== mutant $m: fails, as it must ($(cat "$out/m$m.log" "$out/m$m-env.log" 2>/dev/null | grep -m1 '^FAIL' | cut -c1-150))"
    fi
done
shadow="$root/decomp/src/Game/LiveActor/ShadowController.cpp"
sed -n '/^    const char\* honeyHold =/,/^    sHoneyHoldBatch = false;/p' "$shadow" > "$out/honey-init.inc"
sed -n '/^    sHoneyHoldBatch = sHoneyStage /,/sHoneyHolds.begin();/p' "$shadow" > "$out/honey-batch.inc"
sed -n '/^void ShadowController::updateFarClippingPb(/,/^}/p' "$shadow" > "$out/honey-update.inc"
"$CXX" -std=gnu++17 -O1 -Wall -I"$out" -I"$root/platform/include" "$root/tests/c183_shadow.cpp" -o "$out/shadow" && "$out/shadow" || rc=1
sed -n '/^J3DModel\* ZoomCullModel(/,/^\/\/ GXV_TINYCULL/{ /^\/\/ GXV_TINYCULL/!p; }' "$root/decomp/src/Game/LiveActor/ClippingActorHolder.cpp" > "$out/zoom-census.inc"
"$CXX" -std=gnu++17 -O1 -Wall -I"$out" "$root/tests/c184_zoom_census.cpp" -o "$out/zoom-census" && "$out/zoom-census" || rc=1
if [ -f "$root/tools/test_zoom_clip.sh" ]; then
    bash "$root/tools/test_zoom_clip.sh" > "$out/zc.txt" 2>&1 || { echo "zoom_clip regression FAILED"; tail -5 "$out/zc.txt"; rc=1; }
    grep -c PASS "$out/zc.txt" | sed 's/^/zoom_clip PASS lines: /'
fi
rm -rf "$out"
exit $rc
