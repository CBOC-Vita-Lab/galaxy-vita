#!/usr/bin/env bash
# fs24: GXV_FS_HALF_FOG (half-exact fog lanes, the half analysis taking vertex-fog keys) and GXV_FS_NOCOLOR (colorDead variant): the lanes against the
# double reference, the modelled half mix in 8-bit levels, banding along a ramp, key / text checks, and the generated shaders on Mesa (pixel oracle).
# No game data or Vita executable needed. (g++ + Mesa; KHR_INCLUDE for the GLES / EGL headers if they are not installed.)
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
src="$root/platform/vgx/src"
inc=()
[[ -n "${KHR_INCLUDE:-}" ]] && inc=(-I"$KHR_INCLUDE")
"${CXX:-g++}" -std=c++20 -O1 -g -Wall -Wextra "${inc[@]}" -I"$src" -I"$root/platform/vgx/third_party" -I"$root/tests" \
  "$root/tests/vgx_fs24_gl.cpp" "$src/vgx_shadergen.cpp" \
  "$root/platform/vgx/host/vgx_host_egl.cpp" -lEGL -lGLESv2 -o "$out/vgx_fs24_gl"
run() { env "$@" LIBGL_ALWAYS_SOFTWARE=1 "$out/vgx_fs24_gl" 2>&1 | grep -v '^libunwind'; }
run -u GXV_FS_HALF -u GXV_FS_HALF_FOG -u GXV_FS_NOCOLOR
run GXV_FS_HALF=1 GXV_FS_HALF_FOG=1 GXV_FS_NOCOLOR=1 GXV_FOGVTX=2
