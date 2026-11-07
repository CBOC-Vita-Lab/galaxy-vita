#!/usr/bin/env bash
# GXV_WARM_NOCHURN on the GL stub with vitaGL's finite program table simulated (tests/vgx_warm_nochurn.cpp): without the switch the
# generic warm-up's capacity service deletes programs to link more of its list; with it, no delete and the reserve stays free.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
"${CXX:-g++}" -std=c++17 -O1 -DNDEBUG -D__VGX_VITA__=1 -I"$root/armhf/glstub" -I"$root/tests/autoq_clock_stubs" -I"$root/platform/include" \
  -I"$root/platform/vgx/src" -I"$root/platform/vgx/third_party" "$root/tests/vgx_warm_nochurn.cpp" "$root/platform/vgx/src/vgx_shadergen.cpp" \
  "$root/platform/vgx/src/vgx_autoq.cpp" -o "$out/vgx_warm_nochurn" -lpthread
"$out/vgx_warm_nochurn" churn
"$out/vgx_warm_nochurn" nochurn
