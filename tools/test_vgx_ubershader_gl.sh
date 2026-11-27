#!/usr/bin/env bash
# GXV_UBERSHADER stage 1 (asyncsh31): the fragment ubershader against each key's own text on Mesa (tests/vgx_ubershader_gl.cpp). No game data or Vita
# executable needed; a shader key list (vgx_shaderlist.bin, e.g. seed 1117's) can be added for the real keys and the coverage census.
# (Where the GLES / EGL headers are not installed: CPLUS_INCLUDE_PATH=<khronos include dir>, e.g. /work/khr/include on the agent machine.)
#   tools/test_vgx_ubershader_gl.sh [output-dir] [random-keys] [vgx_shaderlist.bin ...]
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
keys=${2:-400}
shift $(( $# > 2 ? 2 : $# )) || true
mkdir -p "$out"
"${CXX:-g++}" -std=c++20 -O1 -g -Wall -Wextra -I"$root/platform/vgx/src" -I"$root/platform/vgx/third_party" -I"$root/tests" \
  "$root/tests/vgx_ubershader_gl.cpp" "$root/platform/vgx/src/vgx_ubershader.cpp" "$root/platform/vgx/src/vgx_shadergen.cpp" \
  "$root/platform/vgx/host/vgx_host_egl.cpp" -lEGL -lGLESv2 -o "$out/vgx_ubershader_gl"
# the console's shader switches (vita/vgx-game/entry.cpp, the text switches of seedgate31's profile), then the plain generator
for env in "GXV_FS_HALF=1 GXV_FS_TRIM=1 GXV_FS_TRIM_PARTS=47 GXV_TEVWRAP=2 GXV_NODISCARD=1 GXV_FIX_TEVIN=1 GXV_FIX_INDPREV=1 GXV_FIX_LODBIAS=1 GXV_FOGVTX=2 GXV_FOG_BIND=2 GXV_TEVIN_CANON=2 GXV_FS_FINCLAMP=1 GXV_DISCARD_BLEND=1 GXV_IND_VS=1 GXV_TEXVEC2=1 GXV_VSOUT=1" \
           "GXV_FIX_TEVIN=1"; do
  echo "== $env"
  env $env LIBGL_ALWAYS_SOFTWARE=1 "$out/vgx_ubershader_gl" "$keys" "$@" 2>&1 | grep -v '^libunwind' | tee "$out/ubershader.txt"
  grep -q '^PASS' "$out/ubershader.txt"
done
echo "ok: GXV_UBERSHADER (fragment ubershader = the key's text)"
