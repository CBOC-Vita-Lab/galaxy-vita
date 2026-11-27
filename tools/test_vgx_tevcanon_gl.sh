#!/usr/bin/env bash
# GXV_TEVIN_CANON / GXV_FS_PROJ (shader23), GXV_FS_FINCLAMP / GXV_FS_ADEAD (shader23 round 2): the key decision and the texts on Mesa against the texts as before. No game data or Vita
# executable needed. (Where the GLES / EGL headers are not installed: CPLUS_INCLUDE_PATH=<khronos include dir>.)
#   tools/test_vgx_tevcanon_gl.sh [output-dir] [keys]
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
keys=${2:-1500}
mkdir -p "$out"
"${CXX:-g++}" -std=c++20 -O1 -g -Wall -Wextra -I"$root/platform/vgx/src" -I"$root/platform/vgx/third_party" -I"$root/tests" \
  "$root/tests/vgx_tevcanon_gl.cpp" "$root/platform/vgx/src/vgx_shadergen.cpp" "$root/platform/vgx/host/vgx_host_egl.cpp" -lEGL -lGLESv2 -o "$out/vgx_tevcanon_gl"
# the console's shader switches (vita/vgx-game/entry.cpp), then the plain generator
for env in "GXV_FS_HALF=1 GXV_FS_TRIM=1 GXV_FS_TRIM_PARTS=47 GXV_TEVWRAP=2 GXV_NODISCARD=1 GXV_FIX_TEVIN=1 GXV_FIX_INDPREV=1 GXV_FOGVTX=2" \
           "GXV_FIX_TEVIN=1"; do
  echo "== $env"
  env $env LIBGL_ALWAYS_SOFTWARE=1 "$out/vgx_tevcanon_gl" "$keys" 2>&1 | grep -v '^libunwind' | tee "$out/tevcanon.txt"
  grep -q '^PASS' "$out/tevcanon.txt"
done
echo "ok: GXV_TEVIN_CANON / GXV_FS_PROJ / GXV_FS_FINCLAMP / GXV_FS_ADEAD"
