#!/usr/bin/env bash
# halo31 (GXV_HRW_HALO): the rules against a true redraw on a ray-cast scene (tests/halo31_test.cpp), each mutant of the rules (which must FAIL), and the real
# back end's draws on the GL stub (tests/halo31_backend.cpp, the keepdepth31 harness built as the console builds it).
#   tools/test_halo31.sh [out-dir]     with out-dir: the scenarios' pictures as PPMs (first picture, c181 box, depth-keyed box, no box, halo, the fill painted, truth)
#   HALO31_SKIP_BACKEND=1              the rules only (the back end's compile needs about 1.5 GB and a minute)
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=$(mktemp -d "${TMPDIR:-/tmp}/halo31.XXXXXX")
trap 'rm -rf "$out"' EXIT
cxx=${CXX:-c++}
nice -n 10 "$cxx" -std=c++17 -O2 -Wall -Wextra -I"$root/platform/include" "$root/tests/halo31_test.cpp" -o "$out/rules"
"$out/rules" "$@"
"$root/tools/test_shstencil.sh"   # (dm65: the shadow stencil layout and the player's own-pixel mark)
for m in 1 2 3 4 5 6; do
    nice -n 10 "$cxx" -std=c++17 -O2 -DHALO31_MUTANT=$m -I"$root/platform/include" "$root/tests/halo31_test.cpp" -o "$out/mut$m"
    if "$out/mut$m" > "$out/mut$m.txt" 2>&1; then echo "halo31 mutant $m: PASSED (it must fail)"; exit 1; fi
    echo "halo31 mutant $m: fails as it must ($(grep -c 'CHECK FAILED' "$out/mut$m.txt") checks)"
done
if [[ "${HALO31_SKIP_BACKEND:-0}" != 1 ]]; then
    be=${CXX_BACKEND:-clang++}
    flags=(-std=c++17 -O1 -DNDEBUG -w -D__VGX_VITA__=1 -DPORT_RUNTIME_DIAGNOSTICS=0 -include "$root/tests/dma31_glshim.h" -I"$root/armhf/glstub"
           -I"$root/tests/autoq_clock_stubs" -I"$root/platform/include" -I"$root/platform/vgx/src" -I"$root/platform/vgx/third_party" -I"$root/tests")
    nice -n 10 "$be" "${flags[@]}" "$root/tests/halo31_backend.cpp" "$root/platform/vgx/src/vgx_shadergen.cpp" "$root/platform/vgx/src/vgx_autoq.cpp" -o "$out/be" -lpthread
    GXV_LOG=0 "$out/be" | grep -v "^ok  \|^kept:\|^hrw:" || true
    GXV_LOG=0 "$out/be" > "$out/be.txt" 2>&1 || { grep "FAIL" "$out/be.txt"; echo "halo31 back end: FAIL"; exit 1; }
fi
echo "halo31: PASS"
