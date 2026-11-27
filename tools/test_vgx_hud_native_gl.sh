#!/usr/bin/env bash
# GXV_HUD_NATIVE (hud24): the 2D layer at the display's own resolution, through the production GX front end and GLES back end on Mesa
# (tests/vgx_hud_native_gl.cpp; the pictures are compared by tests/vgx_hud_native_check.py). Runs: GXV_HUD_NATIVE=0 / 1 / 2 at the 720x404 rung,
# 0 and 1 at 960x544 (the panel's size: nothing to gain, every picture falls back), 0 at 848x480 and 1 at 848x480.
#   tools/test_vgx_hud_native_gl.sh [output-dir]   (Khronos headers: KHR_INCLUDE, default /work/khr/include)
#   HN_SKIP_BUILD=1 reuses the build;  HN_CXXFLAGS="-fsanitize=address,undefined -O1 -g" builds sanitized (then HN_OBJ=<dir> for the objects)
set -euo pipefail
root=${HN_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}
out=${1:-$(mktemp -d)}
obj=${HN_OBJ:-$out/obj}
mkdir -p "$out" "$obj"
src="$root/platform/vgx/src"
khr=${KHR_INCLUDE:-/work/khr/include}
sources=(vgx_state vgx_draw vgx_fesplit vgx_dlcache vgx_guestmem vgx_bpsync vgx_shadergen vgx_texcache vgx_texdecode vgx_texcomp vgx_stats vgx_backend_gl vgx_autoq)
flags=(-std=c++20 -O1 -g0 ${HN_CXXFLAGS:-} -I"$src" -I"$root/platform/vgx/third_party" -I"$root/platform/include" -I"$khr")
if [[ "${HN_SKIP_BUILD:-0}" != 1 ]]; then
  objs=()
  for unit in "${sources[@]}"; do
    o="$obj/$unit.o"
    newer=0
    for h in "$src/$unit.cpp" "$src/vgx_cmd.h" "$src/vgx_state.h" "$src/vgx_fesplit.h"; do [[ "$h" -nt "$o" ]] && newer=1; done
    if [[ ! -f "$o" || $newer == 1 ]]; then nice -n 10 "${CXX:-g++}" "${flags[@]}" -c "$src/$unit.cpp" -o "$o"; fi
    objs+=("$o")
  done
  nice -n 10 "${CXX:-g++}" "${flags[@]}" -c "$root/tests/vgx_hud_native_gl.cpp" -o "$obj/test.o"
  [[ -f "$obj/host.o" ]] || nice -n 10 "${CXX:-g++}" "${flags[@]}" -c "$root/platform/vgx/host/vgx_host_egl.cpp" -o "$obj/host.o"
  "${CXX:-g++}" ${HN_CXXFLAGS:-} "${objs[@]}" "$obj/test.o" "$obj/host.o" -lEGL -lGLESv2 -pthread -o "$out/vgx_hud_native_gl"
fi
run() { # tag mode width height
  mkdir -p "$out/$1"
  env LIBGL_ALWAYS_SOFTWARE=1 LP_NUM_THREADS=1 GXV_TEXMEMO=0 GXV_LOG=0 GXV_SPLIT=0 GXV_AUTOQ=0 GXV_FXHALF=0 GXV_PASSHALF=0 GXV_CROWD=0 GXV_SOFTXFORM=0 GXV_EFBSWAP=0 GXV_EFBSWAP2=0 \
    GXV_HUD_NATIVE=$2 GXV_HUD_NATIVE_HOLD=${HN_HOLD:-0} HN_RW=$3 HN_RH=$4 HN_OUT="$out/$1" ${5:-} ${HN_EXTRA:-} "$out/vgx_hud_native_gl" > "$out/$1.log" 2>&1 || { echo "FAILED: run $1"; tail -5 "$out/$1.log"; exit 1; }
  grep -q "^FAIL" "$out/$1.log" && { echo "FAILED: $1"; grep "^FAIL" "$out/$1.log"; exit 1; }
  echo "== $1 (GXV_HUD_NATIVE=$2, render $3x$4)"; cat "$out/$1.log"
}
run off720 0 720 404
run on720 1 720 404
run dry720 2 720 404
run nodepth720 1 720 404 HN_NODEPTH=1  # (a host whose display has no depth buffer: depth-tested draws end the tail as in round 1)
run off960 0 960 544
run on960 1 960 544
run off848 0 848 480
run on848 1 848 480
run trans720 1 720 404 "GXV_HUD_NATIVE_HOLD=3 HN_TRANS=1"  # (hud56: hold start / end and a 60 Assist pair's picture: a HUD on every picture, nothing lost)
run trans848 1 848 480 "GXV_HUD_NATIVE_HOLD=3 HN_TRANS=1"
# (fx59: 60 Assist dropping in and out: pairs with singles between and after; after each pair the hold keeps the singles on the fallback path, native comes back after it)
run pairs720 1 720 404 "GXV_HUD_NATIVE_HOLD=3 HN_TRANS=1 HN_SEQ=HHHHHPPPHHHHHHHHPHHPHHHHHPPHHHHHHH"
run pairs848 1 848 480 "GXV_HUD_NATIVE_HOLD=3 HN_TRANS=1 HN_SEQ=HHHHHPPPHHHHHHHHPHHPHHHHHPPHHHHHHH"
python3 "$root/tests/vgx_hud_native_check.py" "$out"
