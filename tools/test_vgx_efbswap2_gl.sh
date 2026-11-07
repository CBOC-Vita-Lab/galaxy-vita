#!/usr/bin/env bash
# GXV_EFBSWAP2 (whole-EFB copies with a conversion the samplers do not see, made without a copy scene; vgx_draw.cpp MarkSwapCopies,
# vgx_backend_gl.cpp EfbSwap / RawPrescan): the production GX front end and GLES back end on Mesa (tests/vgx_efbswap2_gl.cpp).
# Both pictures must be identical with the switch off and on, frame 1 must have two scenes less with it on, and frame 2's reader of
# the alpha the conversion sets must see it (converted first).
#   tools/test_vgx_efbswap2_gl.sh [output-dir]       (no game data, no Vita; Khronos headers: KHR_INCLUDE, default /work/khr/include)
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
src="$root/platform/vgx/src"
khr=${KHR_INCLUDE:-/work/khr/include}
sources=(vgx_state vgx_draw vgx_fesplit vgx_dlcache vgx_guestmem vgx_bpsync vgx_shadergen vgx_texcache vgx_texdecode vgx_stats vgx_backend_gl vgx_autoq)
args=()
for unit in "${sources[@]}"; do args+=("$src/$unit.cpp"); done
if [[ "${EFBSWAP2_SKIP_BUILD:-0}" != 1 ]]; then
  nice -n 10 "${CXX:-g++}" -std=c++20 -O1 ${EFBSWAP2_DEBUG:--g} -I"$src" -I"$root/platform/vgx/third_party" -I"$khr" \
    "$root/tests/vgx_efbswap2_gl.cpp" "${args[@]}" "$root/platform/vgx/host/vgx_host_egl.cpp" \
    -lEGL -lGLESv2 -pthread -o "$out/vgx_efbswap2_gl"
fi
# runs: name and the switches (0: off; old: on as before r86 - no hold, no prediction, planes' own mask; 1: on with the defaults;
# p: prediction alone; h: hold alone; lf: GXV_VGL_LOADFROM, the next scene's colour load emulated by a blit on the desktop)
runs=("0:GXV_EFBSWAP2=0" "old:GXV_EFBSWAP2=1 GXV_EFBSWAP2_HOLD=0 GXV_EFBSWAP2_PREDICT=0 GXV_EFBSWAP_FULLMASK=0"
      "1:GXV_EFBSWAP2=1" "p:GXV_EFBSWAP2=1 GXV_EFBSWAP2_HOLD=0" "h:GXV_EFBSWAP2=1 GXV_EFBSWAP2_PREDICT=0"
      "lf:GXV_EFBSWAP2=1 GXV_VGL_LOADFROM=1")
for r in "${runs[@]}"; do
  v=${r%%:*}
  env LIBGL_ALWAYS_SOFTWARE=1 LP_NUM_THREADS=1 GXV_LOG=0 GXV_SPLIT=0 GXV_AUTOQ=0 GXV_FXHALF=0 GXV_PASSHALF=0 GXV_CROWD=0 \
    GXV_EFBSWAP=1 ${r#*:} "$out/vgx_efbswap2_gl" > "$out/swap2-$v.log" 2>&1 || true; grep -v "^libunwind" "$out/swap2-$v.log" | sed "s/^/[$v] /"
done
pics() { grep -E '^frame' "$out/swap2-$1.log" | sed -E "s/scenes=[0-9]+ //; s/(partial_swaps|converts|adopts)=[0-9]+ //" | sort; }
scenes1() { grep -E '^frame1 ' "$out/swap2-$1.log" | sed -E 's/.*scenes=([0-9]+).*/\1/'; }
grep -q FAIL "$out"/swap2-*.log && { echo "FAILED: a check"; exit 1; }
h0=$(pics 0); s0=$(scenes1 0)
for v in old 1 p h lf; do
  [[ "$h0" == "$(pics $v)" ]] || { echo "FAILED: pictures differ ($v)"; echo "$h0"; pics $v; exit 1; }
  s1=$(scenes1 $v)
  [[ $((s0 - s1)) -eq 2 ]] || { echo "FAILED: frame 1 scenes $s0 -> $s1 (expected two less, $v)"; exit 1; }
done
echo "PASS: identical pictures in all runs (0 old 1 p h lf), frame 1 scenes $s0 -> $(scenes1 1)"
