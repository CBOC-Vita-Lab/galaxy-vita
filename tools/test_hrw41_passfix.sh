#!/usr/bin/env bash
# finish44: GXV_HRW41_MASK_PASSFIX (HRW-DESIGN S2a) in the real back end on the GL stub (tests/hrw41_passfix_backend.cpp), built as the console builds it
# (__VGX_VITA__, PORT_RUNTIME_DIAGNOSTICS=0; tools/test_keepdepth31_backend.sh's harness). One process a case (the switches are read once).
# The pure rule (port::hrw41::PassFixMark) is in tests/hrw41_test.cpp (tools/test_hrw41.sh).
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=$(mktemp -d "${TMPDIR:-/tmp}/hrw41-passfix.XXXXXX")
trap 'rm -rf "$out"' EXIT
cxx=${CXX_BACKEND:-clang++}
flags=(-std=c++17 -O1 -DNDEBUG -w -D__VGX_VITA__=1 -DPORT_RUNTIME_DIAGNOSTICS=0 -include "$root/tests/dma31_glshim.h" -I"$root/armhf/glstub"
       -I"$root/tests/autoq_clock_stubs" -I"$root/platform/include" -I"$root/platform/vgx/src" -I"$root/platform/vgx/third_party" -I"$root/tests")
nice -n 10 "$cxx" "${flags[@]}" "$root/tests/hrw41_passfix_backend.cpp" "$root/platform/vgx/src/vgx_shadergen.cpp" "$root/platform/vgx/src/vgx_autoq.cpp" \
    -o "$out/be" -lpthread
fail=0
for t in fix off depth-touched outside no-detail second-pass; do
    GXV_LOG=0 "$out/be" "$t" || { echo "hrw41 passfix back end case $t: FAIL"; fail=1; }
done
[[ $fail -eq 0 ]] || exit 1
echo "hrw41 passfix back end: PASS"
