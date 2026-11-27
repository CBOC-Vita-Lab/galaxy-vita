#!/usr/bin/env bash
# keepdepth31 (GXV_HRW_KEEPDEPTH / GXV_HRW_BOXDEPTH): host proof.
#   1. tests/keepdepth31_test.cpp, port/keepdepth.h as shipped: must PASS; then once per fault (KEEPDEPTH_MUTANT=1..15; 12-14 emufix31's GXV_HRW_KEEPDEPTH_2D): each must FAIL.
#   2. tests/keepdepth31_backend.cpp: the real back end on the GL stub (built as the console builds it): a pair's first picture leaves its last depth
#      clear out only when nothing after it touches depth, the plan keeps the depth live, the picture after it is cleared for only when it does not
#      overwrite its depth first, and the census / violation counters say so. KEEPDEPTH_BACKEND=0 skips it (it compiles the whole back end).
#   3. tools/test_c182_warp_pixels.sh (needs EGL + GLES2: Mesa's software renderer; CPATH=/work/khr/include where the headers are not installed):
#      kFsBoxDepth itself, drawn, against the plain box and the quarter-mask key. KEEPDEPTH_PIXELS=0 skips it.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=$(mktemp -d "${TMPDIR:-/tmp}/keepdepth31.XXXXXX")
trap 'rm -rf "$out"' EXIT
CXX=${CXX:-c++}
fail=0
"$CXX" -std=c++17 -O1 -Wall -I"$root/platform/include" "$root/tests/keepdepth31_test.cpp" -o "$out/t0"
"$out/t0" || fail=1
for m in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do
    "$CXX" -std=c++17 -O1 -Wall -DKEEPDEPTH_MUTANT=$m -I"$root/platform/include" "$root/tests/keepdepth31_test.cpp" -o "$out/t$m"
    if "$out/t$m" > "$out/m$m.log" 2>&1; then
        echo "mutant $m PASSED: the test cannot see that fault"; fail=1
    else
        echo "== mutant $m fails, as it must: $(grep -m1 '^FAIL' "$out/m$m.log")"
    fi
done
if [[ ${KEEPDEPTH_BACKEND:-1} != 0 ]]; then
    bash "$root/tools/test_keepdepth31_backend.sh" || fail=1
fi
if [[ ${KEEPDEPTH_PIXELS:-1} != 0 ]]; then
    bash "$root/tools/test_c182_warp_pixels.sh" || fail=1
fi
[[ $fail -eq 0 ]] || { echo "keepdepth31: FAIL"; exit 1; }
echo "keepdepth31: PASS"
