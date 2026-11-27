#!/usr/bin/env bash
# GXV_TAILFOLD=3 (tailfold39): a bloom picture's tail folded into the present, through the production GX front end and GLES back end on Mesa (tests/vgx_tailfold_gl.cpp; the pictures are compared by
# tests/vgx_tailfold_check.py). Runs at the 960x544 render size (the panel's: the 1:1 rung): GXV_TAILFOLD=0 / 3 / 1 with the game's EFB 640x456, and 0 / 3 with an EFB of the render size (960x544,
# TF_EFBW/H: coordinates on pixel boundaries and centres, the fill-rule scenario), and at 720x404 (not 1:1: nothing is folded).
#   tools/test_vgx_tailfold_gl.sh [output-dir]   (Khronos headers: KHR_INCLUDE; a host whose compiler rejects the console-only weak declarations of vgx_backend_gl.cpp: GL_TEST_SHIM=<file with
#   `extern "C" void vgxvita_workerAllocStats_v1(uint32_t*, uint32_t*, uint32_t*) __attribute__((weak)); extern "C" uint32_t port_scene_serial(void) __attribute__((weak));`> CXX=clang++;
#   LIBRARY_PATH holding libEGL.so / libGLESv2.so links)
#   TF_SKIP_BUILD=1 reuses the build
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
obj=${TF_OBJ:-$out/obj}
mkdir -p "$out" "$obj"
src="$root/platform/vgx/src"
khr=${KHR_INCLUDE:-/work/khr/include}
shim=()
[[ -n "${GL_TEST_SHIM:-}" ]] && shim=(-include "$GL_TEST_SHIM")
flags=(-std=c++20 -O1 -g0 "${shim[@]}" -I"$src" -I"$root/platform/include" -I"$root/platform/vgx/third_party" -I"$khr")
sources=(vgx_state vgx_draw vgx_fesplit vgx_dlcache vgx_guestmem vgx_bpsync vgx_shadergen vgx_texcache vgx_texcomp vgx_texdecode vgx_stats vgx_backend_gl vgx_autoq)
if [[ "${TF_SKIP_BUILD:-0}" != 1 ]]; then
  objs=()
  for unit in "${sources[@]}"; do
    o="$obj/$unit.o"
    newer=0
    for h in "$src/$unit.cpp" "$src/vgx_cmd.h" "$src/vgx_state.h" "$src/vgx_fesplit.h" "$src/vgx_bloombracket.h" "$src/vgx_tailfold.h"; do [[ "$h" -nt "$o" ]] && newer=1; done
    if [[ ! -f "$o" || $newer == 1 ]]; then nice -n 10 "${CXX:-g++}" "${flags[@]}" -c "$src/$unit.cpp" -o "$o"; fi
    objs+=("$o")
  done
  nice -n 10 "${CXX:-g++}" "${flags[@]}" -c "$root/tests/vgx_tailfold_gl.cpp" -o "$obj/test.o"
  [[ -f "$obj/host.o" ]] || nice -n 10 "${CXX:-g++}" "${flags[@]}" -c "$root/platform/vgx/host/vgx_host_egl.cpp" -o "$obj/host.o"
  "${CXX:-g++}" "${objs[@]}" "$obj/test.o" "$obj/host.o" -lEGL -lGLESv2 -pthread -o "$out/vgx_tailfold_gl"
fi
run() { # tag mode width height efbw efbh
  mkdir -p "$out/$1"
  env LIBGL_ALWAYS_SOFTWARE=1 LP_NUM_THREADS=1 GXV_TEXMEMO=0 GXV_LOG=0 GXV_SPLIT=0 GXV_AUTOQ=0 GXV_FXHALF=0 GXV_PASSHALF=0 GXV_CROWD=0 GXV_HUD_NATIVE=0 \
    GXV_TAILFOLD=$2 TF_RW=$3 TF_RH=$4 TF_EFBW=$5 TF_EFBH=$6 TF_OUT="$out/$1" "$out/vgx_tailfold_gl" > "$out/$1.log" 2>&1 || { echo "FAILED: run $1"; tail -5 "$out/$1.log"; exit 1; }
  grep -q "^FAIL" "$out/$1.log" && { echo "FAILED: $1"; grep "^FAIL" "$out/$1.log"; exit 1; }
  echo "== $1 (GXV_TAILFOLD=$2, render $3x$4, EFB $5x$6)"; grep -v "^libunwind" "$out/$1.log"
}
run off 0 960 544 640 456
run on 3 960 544 640 456
run count 1 960 544 640 456
run off-native 0 960 544 960 544
run on-native 3 960 544 960 544
run off720 0 720 404 640 456
run on720 3 720 404 640 456
python3 "$root/tests/vgx_tailfold_check.py" "$out"
