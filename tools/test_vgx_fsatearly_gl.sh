#!/usr/bin/env bash
# Host GLES2 test for GXV_FS_ATEARLY=1 (v2: early alpha + discard before colour & fog, zero texture fetches after discard).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${TMPDIR:-/tmp}/vgx_fsatearly_gl"
CXX="${CXX:-c++}"
inc=()
[[ -n "${KHR_INCLUDE:-}" ]] && inc=(-I"$KHR_INCLUDE")

"$CXX" -std=c++20 -O2 -Wall -Wextra -Wno-unused-function ${inc[@]+"${inc[@]}"} \
  -I"$ROOT/platform/vgx/src" -I"$ROOT/platform/vgx/third_party" -I"$ROOT/platform/include" -I"$ROOT/tests" \
  "$ROOT/tests/vgx_fsatearly_gl.cpp" "$ROOT/platform/vgx/host/vgx_host_egl.cpp" "$ROOT/platform/vgx/src/vgx_shadergen.cpp" \
  -lEGL -lGLESv2 -o "$OUT"

echo "=== default flags ==="
"$OUT"
echo "=== release flags (FS_HALF=1, FS_TRIM=1, FS_FINCLAMP=1, FS_TEXREUSE=1, FS_PROJ=1, TEVIN_CANON=2, FS_HALF_FOG=1) ==="
GXV_FS_HALF=1 GXV_FS_TRIM=1 GXV_FS_TRIM_PARTS=15 GXV_FS_FINCLAMP=1 GXV_FS_TEXREUSE=1 GXV_FS_PROJ=1 GXV_TEVIN_CANON=2 GXV_FS_HALF_FOG=1 "$OUT"
echo "=== with FS_ATFAST=1 ==="
GXV_FS_ATFAST=1 GXV_FS_HALF=1 GXV_FS_FINCLAMP=1 GXV_FS_TEXREUSE=1 "$OUT"
