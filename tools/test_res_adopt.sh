#!/usr/bin/env bash
# stall27 (GXV_RES_ADOPT): back end on the GL stub (premade copy targets adopted / taken at first use).   bash tools/test_res_adopt.sh [out-dir]
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d "${TMPDIR:-/tmp}/resadopt.XXXXXX")}
mkdir -p "$out"
FLAGS=(-std=c++17 -O1 -DNDEBUG -fpermissive -w -D__VGX_VITA__=1 -include "$root/tests/asynclink_glshim.h" -I"$root/armhf/glstub" -I"$root/tests/autoq_clock_stubs" -I"$root/platform/include"
  -I"$root/platform/vgx/src" -I"$root/platform/vgx/third_party")
SRCS=("$root/platform/vgx/src/vgx_shadergen.cpp" "$root/platform/vgx/src/vgx_autoq.cpp")
"${CXX:-g++}" "${FLAGS[@]}" "$root/tests/vgx_res_adopt_backend.cpp" "${SRCS[@]}" -o "$out/vgx_res_adopt_backend" -lpthread
"$out/vgx_res_adopt_backend"
