#!/usr/bin/env bash
# GXV_IND_PRECOMP (gpu23b, a MINOR CHANGE): Beach Bowl's sea and random indirect keys drawn with the precomputed indirect coordinate and
# without, real generated shaders on Mesa (tests/vgx_indpre_gl.cpp); the sea's picture must be within one level, all pictures together
# may differ in at most 0.1% of their bytes (rare pixels of materials that compare or wrap the warped sample). Also: with the switch off
# the generator's texts are those of the base tree (tests/vgx_indvs_gl.cpp's switch-off digest, built against REF_ROOT, default
# /home/claude/g23/source/source).
#   tools/test_vgx_indpre_gl.sh [output-dir] [pairs]
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
pairs=${2:-300}
ref=${REF_ROOT:-/home/claude/g23/source/source}
khr=${KHR_INCLUDE:-/work/khr/include}
mkdir -p "$out"
cc() { nice -n 10 "${CXX:-g++}" -pipe -std=c++20 -O1 -g0 -I"$1/platform/vgx/src" -I"$1/platform/vgx/third_party" -I"$root/tests" -I"$khr" "${@:3}" \
  "$1/platform/vgx/src/vgx_shadergen.cpp" "$1/platform/vgx/host/vgx_host_egl.cpp" -lEGL -lGLESv2 -o "$2"; }
cc "$root" "$out/vgx_indpre_gl" "$root/tests/vgx_indpre_gl.cpp"
cc "$root" "$out/vgx_indvs_new" -DINDVS_REF "$root/tests/vgx_indvs_gl.cpp"
cc "$ref" "$out/vgx_indvs_ref" -DINDVS_REF "$root/tests/vgx_indvs_gl.cpp"
fail=0
for fs in "GXV_FS_TRIM=1 GXV_FS_HALF=1" "GXV_FS_TRIM=0 GXV_FS_HALF=0"; do
  for indvs in 1 0; do for tsz in 37 32 640; do
    line=$(env IP_TSZ=$tsz LIBGL_ALWAYS_SOFTWARE=1 GXV_IND_PRECOMP=1 GXV_IND_VS=$indvs GXV_TEVWRAP=2 GXV_FIX_TEVIN=1 GXV_FIX_INDPREV=0 $fs "$out/vgx_indpre_gl" "$pairs" 2>&1 | grep -v '^libunwind' | tail -2 || true)
    echo "[$fs IND_VS=$indvs] $line"
    echo "$line" | grep -q "^FAIL" && fail=1
  done; done
  a=$(env LIBGL_ALWAYS_SOFTWARE=1 GXV_IND_PRECOMP=0 GXV_TEVWRAP=2 GXV_FIX_TEVIN=1 $fs "$out/vgx_indvs_new" "$pairs" 2>&1 | grep -o "off-digest=[0-9a-f]*")
  b=$(env LIBGL_ALWAYS_SOFTWARE=1 GXV_TEVWRAP=2 GXV_FIX_TEVIN=1 $fs "$out/vgx_indvs_ref" "$pairs" 2>&1 | grep -o "off-digest=[0-9a-f]*")
  echo "[$fs] switch off: $a, base tree: $b"
  [[ "$a" == "$b" ]] || { echo "FAIL: texts with GXV_IND_PRECOMP=0 differ from the base tree"; fail=1; }
done
[[ $fail -eq 0 ]] || { echo "FAILED"; exit 1; }
echo "PASS: the sea within one level, all pictures within 0.1% of their bytes with GXV_IND_PRECOMP=1; texts unchanged with it off"
