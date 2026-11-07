#!/usr/bin/env bash
# GXV_IND_VS (an indirect lookup's coordinate scale in the vertex shader, the lookup a non-dependent read: IndVsPair in
# vgx_shadergen.cpp): pictures, the interpolated coordinate's bits and a one-ulp control on Mesa (tests/vgx_indvs_gl.cpp), with the
# other pair switches off and on. No game data or Vita executable needed.
#   tools/test_vgx_indvs_gl.sh [output-dir] [pairs]
# REF_ROOT=<tree before the switch> also builds the test against that tree's generator and requires the same switch-off texts.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
pairs=${2:-600}
mkdir -p "$out"
build() { # tree output [defines]
  local tree=$1 bin=$2; shift 2
  "${CXX:-g++}" -std=c++20 -O1 -g -Wall -Wextra "$@" -I"$tree/platform/vgx/src" -I"$tree/platform/vgx/third_party" -I"$root/tests" \
    "$root/tests/vgx_indvs_gl.cpp" "$tree/platform/vgx/src/vgx_shadergen.cpp" "$tree/platform/vgx/host/vgx_host_egl.cpp" -lEGL -lGLESv2 -o "$bin"
}
run() { local bin=$1; shift; env "$@" LIBGL_ALWAYS_SOFTWARE=1 "$bin" "$pairs" 2>&1 | grep -v '^libunwind'; }
build "$root" "$out/vgx_indvs_gl"
for sw in "GXV_TEXVEC2=0 GXV_VSOUT=0" "GXV_TEXVEC2=1 GXV_VSOUT=0" "GXV_TEXVEC2=1 GXV_VSOUT=1"; do
  for fs in "GXV_FS_TRIM=1 GXV_FS_HALF=1" "GXV_FS_TRIM=0 GXV_FS_HALF=0"; do
    tag=$(echo "$sw $fs" | tr -dc '01')
    echo "== $sw $fs"
    # shellcheck disable=SC2086
    run "$out/vgx_indvs_gl" GXV_IND_VS=1 GXV_TEVWRAP=2 GXV_FIX_TEVIN=1 GXV_FIX_INDPREV=0 $sw $fs > "$out/on-$tag.txt"
    tail -1 "$out/on-$tag.txt"
    grep -q "all equal" "$out/on-$tag.txt"
  done
done
if [ -n "${REF_ROOT:-}" ]; then
  build "$REF_ROOT" "$out/vgx_indvs_ref" -DINDVS_REF
  for sw in "GXV_TEXVEC2=0 GXV_VSOUT=0" "GXV_TEXVEC2=1 GXV_VSOUT=1"; do
    tag=$(echo "$sw GXV_FS_TRIM=1 GXV_FS_HALF=1" | tr -dc '01')
    # shellcheck disable=SC2086
    run "$out/vgx_indvs_ref" GXV_TEVWRAP=2 GXV_FIX_TEVIN=1 $sw GXV_FS_TRIM=1 GXV_FS_HALF=1 > "$out/ref-$tag.txt"
    a=$(grep -o "off-digest=[0-9a-f]*" "$out/on-$tag.txt"); b=$(grep -o "off-digest=[0-9a-f]*" "$out/ref-$tag.txt")
    [ "$a" = "$b" ] || { echo "FAIL: switch-off texts differ from the reference tree ($sw): $a vs $b"; exit 1; }
    echo "reference tree ($sw): same switch-off texts ($a)"
  done
fi
echo "ok: GXV_IND_VS pictures and coordinates equal"
