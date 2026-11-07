#!/usr/bin/env bash
# GXV_PX_NOOP / GXV_PX_DEADCLEAR / GXV_PX_SCISSOR (vgx_backend_gl.cpp, "pixcut"): the production GX front end and GLES back end on Mesa
# (tests/vgx_pxcut_gl.cpp). For each render size (the EFB's 64x64 and a non-integer 72x81 scale) every switch combination must give
# pictures identical (all channels) to the switches-off run, and each switch that is on must have cut what the test expects.
#   tools/test_vgx_pxcut_gl.sh [output-dir]      (no game data, no Vita; Khronos headers: KHR_INCLUDE, default /work/khr/include)
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
src="$root/platform/vgx/src"
khr=${KHR_INCLUDE:-/work/khr/include}
sources=(vgx_state vgx_draw vgx_fesplit vgx_dlcache vgx_guestmem vgx_bpsync vgx_shadergen vgx_texcache vgx_texdecode vgx_stats vgx_backend_gl vgx_autoq)
args=()
for unit in "${sources[@]}"; do args+=("$src/$unit.cpp"); done
if [[ "${PXCUT_SKIP_BUILD:-0}" != 1 ]]; then
  nice -n 10 "${CXX:-g++}" -std=c++20 -O1 -g ${PXCUT_CXXFLAGS:-} -I"$src" -I"$root/platform/vgx/third_party" -I"$khr" \
    "$root/tests/vgx_pxcut_gl.cpp" "${args[@]}" "$root/platform/vgx/host/vgx_host_egl.cpp" \
    -lEGL -lGLESv2 -pthread -o "$out/vgx_pxcut_gl"
fi
fail=0
for size in "64 64" "72 81"; do
  set -- $size
  ref=""
  for sw in "0 0 0" "1 0 0" "0 1 0" "0 0 1" "1 1 1"; do
    read -r a b c <<< "$sw"
    tag="$1x$2-$a$b$c"
    env LIBGL_ALWAYS_SOFTWARE=1 LP_NUM_THREADS=1 GXV_LOG=0 GXV_SPLIT=0 GXV_AUTOQ=0 GXV_FXHALF=0 GXV_PASSHALF=0 GXV_CROWD=0 \
      PXCUT_RW=$1 PXCUT_RH=$2 GXV_PX_NOOP=$a GXV_PX_DEADCLEAR=$b GXV_PX_SCISSOR=$c "$out/vgx_pxcut_gl" > "$out/pxcut-$tag.log" 2>&1 || fail=1
    grep -v "^libunwind" "$out/pxcut-$tag.log" | grep -E "FAIL|^cuts|^picture" | sort -u | sed "s/^/$tag: /"
    h=$(grep -E '^frame[0-9] hash' "$out/pxcut-$tag.log" | tr '\n' ' ')
    if [[ -z "$ref" ]]; then ref="$h"; echo "$tag: $h"; elif [[ "$h" != "$ref" ]]; then echo "FAILED: $tag pictures differ: $h vs $ref"; fail=1; fi
  done
done
grep -l FAIL "$out"/pxcut-*.log && fail=1
[[ $fail -eq 0 ]] || { echo "FAILED"; exit 1; }
echo "PASS: identical pictures with every GXV_PX_* switch, cuts made"
