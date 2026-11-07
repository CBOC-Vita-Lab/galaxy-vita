#!/usr/bin/env bash
# GXV_FS_TRIM / GXV_VS_TRIM (from the Strikers port's 1.3 shader trims): the range analysis on a CPU model of the TEV pipeline, the half
# precision bound with the trimmed clamps, and the full against the trimmed texts on Mesa (fragment and vertex). No game data or Vita
# executable needed.
#   tools/test_vgx_trim_gl.sh [output-dir] [keys]
# The switch-off texts are covered by tools/test_vgx_vsout_gl.sh with REF_ROOT (vertex and fragment digests against a tree before the
# switches) and tools/test_vgx_fshalf.sh (the fragment digest pinned since GXV_FS_HALF).
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
keys=${2:-1500}
mkdir -p "$out"
"${CXX:-g++}" -std=c++20 -O1 -g -Wall -Wextra -I"$root/platform/vgx/src" -I"$root/platform/vgx/third_party" -I"$root/tests" \
  "$root/tests/vgx_trim_gl.cpp" "$root/platform/vgx/src/vgx_shadergen.cpp" "$root/platform/vgx/host/vgx_host_egl.cpp" -lEGL -lGLESv2 -o "$out/vgx_trim_gl"
for wrap in 2 0; do
  for lt in 1 0; do
    echo "== GXV_TEVWRAP=$wrap GXV_LTMERGE=$lt"
    env GXV_FS_TRIM=1 GXV_FS_HALF=1 GXV_TEVWRAP=$wrap GXV_LTMERGE=$lt LIBGL_ALWAYS_SOFTWARE=1 "$out/vgx_trim_gl" "$keys" 2>&1 | grep -v '^libunwind' \
      | tee "$out/trim-w$wrap-lt$lt.txt"
    grep -q '^PASS' "$out/trim-w$wrap-lt$lt.txt"
  done
done
echo "ok: GXV_FS_TRIM / GXV_VS_TRIM"
