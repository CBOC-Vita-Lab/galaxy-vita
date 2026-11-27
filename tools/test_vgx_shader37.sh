#!/usr/bin/env bash
# shader37: GXV_FS_ATFAST and GXV_VS_NOBRANCH. Host test: the generated GLES2 shaders on Mesa (pixel oracle) and CPU models; no game data, no Vita.
# Optional second argument: a vgx_shaderlist.bin; the runner then also checks, for every key of the list, that with both switches off the text and
# the u_tev row count are byte-identical to the generator at the commit before the switches (git rev given by BASE_REV, default HEAD~1 is NOT assumed:
# pass BASE_REV=<commit before shader37>), and reports how many of its keys each switch changes.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
list=${2:-}
mkdir -p "$out"
src="$root/platform/vgx/src"
inc=()
[ -n "${GLES_SHIM:-}" ] && inc=(-I"$GLES_SHIM") # (a directory with EGL/egl.h and GLES2/gl2.h when the system has none)
"${CXX:-g++}" -std=c++20 -O1 -g -Wall -Wextra -I"$src" -I"$root/platform/vgx/third_party" "${inc[@]}" \
  "$root/tests/vgx_shader37_gl.cpp" "$src/vgx_shadergen.cpp" ${GLES_LIBS:--lEGL -lGLESv2} -o "$out/vgx_shader37_gl"
run() { env "$@" LIBGL_ALWAYS_SOFTWARE=1 "$out/vgx_shader37_gl" 2>&1 | grep -v '^libunwind'; }
run -u GXV_FS_HALF
run GXV_FS_HALF=1
if [ -n "${list}" ] && [ -n "${BASE_REV:-}" ]; then
  mkdir -p "$out/base"
  git -C "$root" archive "$BASE_REV" platform/vgx | tar -x -C "$out/base"
  "${CXX:-g++}" -std=c++20 -O1 -I"$out/base/platform/vgx/src" -I"$out/base/platform/vgx/third_party" "$root/tests/vgx_shader37_digest.cpp" \
    "$out/base/platform/vgx/src/vgx_shadergen.cpp" -o "$out/digest_base"
  "${CXX:-g++}" -std=c++20 -O1 -I"$src" -I"$root/platform/vgx/third_party" -DSHADER37_NEW "$root/tests/vgx_shader37_digest.cpp" "$src/vgx_shadergen.cpp" -o "$out/digest_new"
  for e in "GXV_FS_HALF=1 GXV_FOGVTX=2 GXV_IND_VS=1 GXV_TEXVEC2=1 GXV_VSOUT=1 GXV_FS_FINCLAMP=1" "GXV_FS_HALF=0"; do
    a=$(env $e "$out/digest_base" "$list"); b=$(env $e "$out/digest_new" "$list")
    [ "$a" = "$b" ] || { echo "FAIL: switches off: the generator's output differs from $BASE_REV ($e)"; echo "$a"; echo "$b"; exit 1; }
    echo "ok: both switches off: byte-identical to $BASE_REV ($a)"
  done
  env GXV_FS_HALF=1 GXV_FS_ATFAST=1 "$out/digest_new" "$list" --changed
  env GXV_FS_HALF=1 GXV_FS_ATFAST=2 "$out/digest_new" "$list" --changed
  env GXV_FS_HALF=1 GXV_VS_NOBRANCH=1 "$out/digest_new" "$list" --changed
fi
echo "ok: test_vgx_shader37"
