#!/usr/bin/env bash
# GXV_VSOUT / GXV_TEXVEC2: vertex shaders that leave out what their fragment shader never reads, and vec2 texture coordinate
# varyings, against the shaders of the same keys without the marks. Real generated GLES2 shaders on Mesa (pixel oracle). No game
# data or Vita executable needed.
#   tools/test_vgx_vsout_gl.sh [output-dir] [pairs]
# REF_ROOT=<tree before the switches> also builds the test against that tree's generator and requires the same switch-off texts.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
pairs=${2:-1500}
mkdir -p "$out"
build() { # tree output [defines]
  local tree=$1 bin=$2; shift 2
  "${CXX:-g++}" -std=c++20 -O1 -g -Wall -Wextra "$@" -I"$tree/platform/vgx/src" -I"$tree/platform/vgx/third_party" -I"$root/tests" \
    "$root/tests/vgx_vsout_gl.cpp" "$tree/platform/vgx/src/vgx_shadergen.cpp" "$tree/platform/vgx/host/vgx_host_egl.cpp" -lEGL -lGLESv2 -o "$bin"
}
run() { local bin=$1; shift; env "$@" LIBGL_ALWAYS_SOFTWARE=1 "$bin" "$pairs" 2>&1 | grep -v '^libunwind'; }
build "$root" "$out/vgx_vsout_gl"
for lt in 0 1; do
  for sw in "GXV_VSOUT=1 GXV_TEXVEC2=0" "GXV_VSOUT=0 GXV_TEXVEC2=1" "GXV_VSOUT=1 GXV_TEXVEC2=1"; do
    tag="lt$lt-$(echo "$sw" | tr -dc '01')"
    # shellcheck disable=SC2086
    run "$out/vgx_vsout_gl" GXV_LTMERGE=$lt $sw > "$out/on-$tag.txt"
    tail -1 "$out/on-$tag.txt"
    grep -q "all equal" "$out/on-$tag.txt"
  done
done
if [ -n "${REF_ROOT:-}" ]; then
  build "$REF_ROOT" "$out/vgx_vsout_ref" -DVSOUT_REF
  for lt in 0 1; do
    run "$out/vgx_vsout_ref" GXV_LTMERGE=$lt > "$out/ref-lt$lt.txt"
    a=$(grep -o "off-digest=[0-9a-f]*" "$out/on-lt$lt-11.txt"); b=$(grep -o "off-digest=[0-9a-f]*" "$out/ref-lt$lt.txt")
    [ "$a" = "$b" ] || { echo "FAIL: switch-off texts differ from the reference tree (GXV_LTMERGE=$lt): $a vs $b"; exit 1; }
    echo "reference tree, GXV_LTMERGE=$lt: same switch-off texts ($a)"
  done
fi
echo "ok: GXV_VSOUT / GXV_TEXVEC2 pictures equal"
