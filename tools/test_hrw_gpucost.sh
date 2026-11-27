#!/usr/bin/env bash
# hrw_gpucost: unit + GLES2 pixel-oracle tests for 04-hrw-gpucost.diff
# (GXV_HRW_SCENE_SWAP_PLAIN, GXV_HRW_SNAP_SWAP, GXV_HRW_OWN_SCISSOR).
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
inc=()
[[ -n "${KHR_INCLUDE:-}" ]] && inc=(-I"$KHR_INCLUDE")
"${CXX:-g++}" -std=c++20 -O1 -g -Wall -Wextra ${inc[@]+"${inc[@]}"} -I"$root/platform/include" -I"$root/platform/vgx/src" \
  "$root/tests/hrw_gpucost_gl.cpp" "$root/platform/vgx/host/vgx_host_egl.cpp" -lEGL -lGLESv2 -o "$out/hrw_gpucost_gl"
LIBGL_ALWAYS_SOFTWARE=1 "$out/hrw_gpucost_gl" 2>&1 | grep -v '^libunwind'
