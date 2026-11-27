#!/usr/bin/env bash
# keepdepth31: GXV_HRW_KEEPDEPTH in the real back end on the GL stub (tests/keepdepth31_backend.cpp), built as the console builds it
# (__VGX_VITA__, PORT_RUNTIME_DIAGNOSTICS=0; tools/test_dma31.sh's harness). One process a case (the switches are read once).
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=$(mktemp -d "${TMPDIR:-/tmp}/keepdepth31-be.XXXXXX")
trap 'rm -rf "$out"' EXIT
cxx=${CXX_BACKEND:-clang++}
flags=(-std=c++17 -O1 -DNDEBUG -w -D__VGX_VITA__=1 -DPORT_RUNTIME_DIAGNOSTICS=0 -include "$root/tests/dma31_glshim.h" -I"$root/armhf/glstub"
       -I"$root/tests/autoq_clock_stubs" -I"$root/platform/include" -I"$root/platform/vgx/src" -I"$root/platform/vgx/third_party" -I"$root/tests")
nice -n 10 "$cxx" "${flags[@]}" "$root/tests/keepdepth31_backend.cpp" "$root/platform/vgx/src/vgx_shadergen.cpp" "$root/platform/vgx/src/vgx_autoq.cpp" \
    -o "$out/be" -lpthread
fail=0
for t in kept off census read-after write-after colour next-live unarmed mirror always-hud watch write2d write2d-off write2d-read write2d-next zclear-nomark hud-reader hud-reader-off hud-reader-two hud-reader-write; do
    GXV_LOG=0 "$out/be" "$t" || { echo "keepdepth31 back end case $t: FAIL"; fail=1; }
done
[[ $fail -eq 0 ]] || exit 1
echo "keepdepth31 back end: PASS"
