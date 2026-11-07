#!/usr/bin/env bash
# GXV_DISCARD_BLEND (no `discard` for an alpha test whose failing fragments a zero source makes no-ops: FsKey::rejectZero,
# RejectIsNoop in vgx_cmd.h, RejectZeroFs in vgx_draw.cpp): the truth table of RejectIsNoop, the texts, and the `discard` against the
# rejectZero pictures on Mesa (tests/vgx_discard_blend_gl.cpp). No game data or Vita executable needed.
#   tools/test_vgx_discard_blend_gl.sh [output-dir] [keys]
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
keys=${2:-300}
mkdir -p "$out"
src="$root/platform/vgx/src"
"${CXX:-g++}" -std=c++20 -O1 -g -Wall -Wextra -I"$src" -I"$root/platform/vgx/third_party" -I"$root/tests" \
  "$root/tests/vgx_discard_blend_gl.cpp" "$src/vgx_shadergen.cpp" "$root/platform/vgx/host/vgx_host_egl.cpp" -lEGL -lGLESv2 \
  -o "$out/vgx_discard_blend_gl"
for trim in 1 0; do
  for half in 1 0; do
    echo "== GXV_FS_TRIM=$trim GXV_FS_HALF=$half"
    env GXV_DISCARD_BLEND=1 GXV_FS_TRIM=$trim GXV_FS_HALF=$half GXV_TEVWRAP=2 GXV_FIX_TEVIN=1 GXV_NODISCARD=1 LIBGL_ALWAYS_SOFTWARE=1 \
      "$out/vgx_discard_blend_gl" "$keys" 2>&1 | grep -v '^libunwind' | tee "$out/db-t$trim-h$half.txt"
    grep -q '^PASS' "$out/db-t$trim-h$half.txt"
  done
done
echo "ok: GXV_DISCARD_BLEND"
