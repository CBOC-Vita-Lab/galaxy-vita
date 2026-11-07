#!/usr/bin/env bash
# The effects' corner tile (tests/vgx_cornerfx_gl.cpp: BloomEffectSimple's save / scratch / put-back of the top-left tile) through the
# production front and back end on Mesa, with the desktop's viewport made as vitaGL makes it (GXV_VGL_VIEWPORT=1: an odd width or
# height loses its last column / row). For every render size:
#   - the default (GXV_RECT_LEGACY=0): the picture equals, byte for byte, the one of the desktop's exact viewport, with no black row;
#   - GXV_RECT_LEGACY=1 at 720x404 and 544x388 (a 101 / 97-row corner): the black row is there (the console's line).
#   tools/test_vgx_cornerfx_gl.sh [output-dir]      (no game data, no Vita; Khronos headers: KHR_INCLUDE, default /usr/include)
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
src="$root/platform/vgx/src"
khr=${KHR_INCLUDE:-/usr/include}
sources=(vgx_state vgx_draw vgx_fesplit vgx_dlcache vgx_guestmem vgx_bpsync vgx_shadergen vgx_texcache vgx_texdecode vgx_stats vgx_backend_gl vgx_autoq)
args=()
for unit in "${sources[@]}"; do args+=("$src/$unit.cpp"); done
if [[ "${CORNERFX_SKIP_BUILD:-0}" != 1 ]]; then
  nice -n 10 "${CXX:-g++}" -std=c++20 -O1 -g -I"$src" -I"$root/platform/vgx/third_party" -I"$khr" \
    "$root/tests/vgx_cornerfx_gl.cpp" "${args[@]}" "$root/platform/vgx/host/vgx_host_egl.cpp" \
    -lEGL -lGLESv2 -pthread -o "$out/vgx_cornerfx_gl"
fi
run() { # size mode env...
  local w=$1 h=$2 mode=$3; shift 3
  env LIBGL_ALWAYS_SOFTWARE=1 LP_NUM_THREADS=1 GXV_LOG=0 GXV_SPLIT=0 GXV_AUTOQ=0 GXV_FXHALF=0 GXV_PASSHALF=0 GXV_CROWD=0 \
    GXV_PX_NOOP=1 GXV_PX_DEADCLEAR=1 GXV_PX_SCISSOR=1 "$@" "$out/vgx_cornerfx_gl" "$w" "$h" "$mode" 2>&1 | grep "^render" || true
}
fail=0
for size in "640 456" "720 404" "664 376" "608 346" "960 544" "544 388"; do
  for mode in 0 1; do
    exact=$(run $size $mode)
    vgl=$(run $size $mode GXV_VGL_VIEWPORT=1)
    echo "exact GL:     $exact"
    echo "vitaGL-like:  $vgl"
    he=$(sed 's/.*hash \([0-9a-f]*\).*/\1/' <<< "$exact"); hv=$(sed 's/.*hash \([0-9a-f]*\).*/\1/' <<< "$vgl")
    [[ -n "$he" && "$he" == "$hv" ]] || { echo "FAILED: $size mode $mode: the vitaGL-like viewport changes the picture"; fail=1; }
    grep -q "black rows 0 " <<< "$exact" || { echo "FAILED: $size mode $mode: black rows"; fail=1; }
    grep -q "black rows 0 " <<< "$vgl" || { echo "FAILED: $size mode $mode: black rows (vitaGL-like)"; fail=1; }
  done
done
for size in "720 404" "544 388"; do
  legacy=$(run $size 0 GXV_VGL_VIEWPORT=1 GXV_RECT_LEGACY=1)
  echo "legacy:       $legacy"
  grep -q "black rows 0 " <<< "$legacy" && { echo "FAILED: $size: GXV_RECT_LEGACY=1 should show the console's black row"; fail=1; }
done
[[ $fail -eq 0 ]] || { echo "FAILED"; exit 1; }
echo "PASS: the corner comes back whole with vitaGL's viewport; GXV_RECT_LEGACY=1 shows the line"
