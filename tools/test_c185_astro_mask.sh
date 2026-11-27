#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
if [[ $# -gt 0 ]]; then
    out=$1
    mkdir -p "$out"
else
    out=$(mktemp -d "${TMPDIR:-/tmp}/c185-astro-mask.XXXXXX")
    trap 'rm -rf "$out"' EXIT
fi
FLAGS=(-std=c++17 -O1 -DNDEBUG -fpermissive -w -D__VGX_VITA__=1 -I"$root/armhf/glstub" -I"$root/tests/autoq_clock_stubs" -I"$root/platform/include"
  -I"$root/platform/vgx/src" -I"$root/platform/vgx/third_party")
SRCS=("$root/platform/vgx/src/vgx_shadergen.cpp" "$root/platform/vgx/src/vgx_autoq.cpp")
# (tests31) the back end needs clang++, not the CXX that tools/run_host_tests.sh exports (g++ 13 refuses its extern "C" port_scene_serial: see host_backend_cxx)
. "$root/tools/host_san.sh"
cxx=$(host_backend_cxx)
"$cxx" "${FLAGS[@]}" "$root/tests/c185_astro_mask_backend.cpp" "${SRCS[@]}" -o "$out/c185_astro_mask_backend" -lpthread
for test in default off prewarm-off no-consumer consumer-disabled no-mask-program quality non-astro hrw-off depth-off stage-disabled msaa overscan-x overscan-y small-width small-height box-only half limits bytecap adopt owned reservation generic-same-mask expiry closed-only drs stage-change eligibility-change eligibility-replan unknown unknown-overfree unknown-have unknown-c184 unknown-have-c184 lowmem allocfail fbo-fail tex-fail fbo-sentinel tex-sentinel; do
    "$out/c185_astro_mask_backend" "$test"
done
