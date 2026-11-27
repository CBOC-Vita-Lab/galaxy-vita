#!/usr/bin/env bash
# GXV_FS_TEXREUSE (gpucut23): repeated texture reads left out of the fragment text; byte-identical pictures on Mesa. No game data or Vita
# executable needed. (Where the GLES / EGL headers are not installed: CPLUS_INCLUDE_PATH=<khronos include dir>.)
#   tools/test_vgx_texreuse_gl.sh [output-dir] [keys] [key-dir: FsKey files (*.key), e.g. a stage's keys]
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
keys=${2:-1500}
keydir=${3:-}
mkdir -p "$out"
"${CXX:-g++}" -std=c++20 -O1 -g -Wall -Wextra -I"$root/platform/vgx/src" -I"$root/platform/vgx/third_party" -I"$root/tests" \
  "$root/tests/vgx_texreuse_gl.cpp" "$root/platform/vgx/src/vgx_shadergen.cpp" "$root/platform/vgx/host/vgx_host_egl.cpp" -lEGL -lGLESv2 -o "$out/vgx_texreuse_gl"
# the console's shader switches (vita/vgx-game/entry.cpp, without the TEXVEC2 / VSOUT pairing, which needs the generated vertex shader),
# with TEVIN_CANON 1 and 2, then the plain generator
for env in "GXV_FS_HALF=1 GXV_FS_TRIM=1 GXV_FS_TRIM_PARTS=47 GXV_TEVWRAP=2 GXV_NODISCARD=1 GXV_FIX_TEVIN=1 GXV_FIX_INDPREV=1 GXV_FOGVTX=2 GXV_FS_FINCLAMP=1 GXV_DISCARD_BLEND=1 GXV_IND_VS=1 GXV_TEVIN_CANON=1" \
           "GXV_FS_HALF=1 GXV_FS_TRIM=1 GXV_FS_TRIM_PARTS=47 GXV_TEVWRAP=2 GXV_NODISCARD=1 GXV_FIX_TEVIN=1 GXV_FIX_INDPREV=1 GXV_FOGVTX=2 GXV_FS_FINCLAMP=1 GXV_DISCARD_BLEND=1 GXV_IND_VS=1 GXV_TEVIN_CANON=2 GXV_FS_PROJ=1" \
           "GXV_FIX_TEVIN=1" ""; do
  echo "== ${env:-(no switches)}"
  env $env LIBGL_ALWAYS_SOFTWARE=1 "$out/vgx_texreuse_gl" "$keys" $keydir 2>&1 | grep -v '^libunwind' | tee "$out/texreuse.txt"
  grep -q '^PASS' "$out/texreuse.txt"
done
echo "ok: GXV_FS_TEXREUSE"
