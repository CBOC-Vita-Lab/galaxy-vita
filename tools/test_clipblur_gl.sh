#!/usr/bin/env bash
# PETARI_CLIPCHAIN_ONEPASS / _SKIPEMPTY / _TAPS host proof: the production FallOutFieldDraw::draw() + ImageEffectLocalUtil.cpp (this tree's and
# CB_BASE_REV's, default 7c5feea) on the console's GX library, the vgx front end and the GLES back end on Mesa (tests/clipblur_gl_{game,main}.cpp).
#   tools/test_clipblur_gl.sh [out-dir]    env: CB_BUILD_ONLY=1, CB_FRAMES, KHR_INCLUDE
# Checks: switches off = the base's pictures bit for bit; modes 1/2: fewer scenes; picture closeness (tools/clipblur_cmp.py); SKIPEMPTY.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
khr=${KHR_INCLUDE:-/work/khr/include}
src="$root/platform/vgx/src"
W="-w -Wno-register"
CW="-w -Wno-implicit-function-declaration -Wno-int-conversion -Wno-incompatible-pointer-types -Wno-incompatible-function-pointer-types"
GINC=(-DNDEBUG=1 -DTARGET_PC=1 -DVERSION=0 -I"$root/vita/include" -I"$root/platform/include" -I"$root/decomp/include" -I"$root/decomp/libs/JSystem/include"
      -I"$root/decomp/libs/RVL_SDK/include" -I"$root/decomp/libs/nw4r/include" -fsigned-char -fno-strict-aliasing -fwrapv -fms-extensions -ffunction-sections
      -fdata-sections -include "$root/vita/include/vita_source_prelude.h" -include "$root/platform/include/port/compat.h" -fno-pie)
CC=${CC:-clang}; CXX=${CXX:-clang++}
build() { # build <tree> <objdir> <exe>
  local tree=$1 o=$2 exe=$3
  mkdir -p "$o"
  local objs=()
  for u in GXAttr GXBump GXDisplayList GXFrameBuf GXGeometry GXInit GXLight GXMisc GXPixel GXTev GXTexture GXTransform; do
    [[ -f "$out/gx/$u.o" ]] || { mkdir -p "$out/gx"; nice "$CC" -std=gnu11 -O1 -c "${GINC[@]}" $CW "$root/decomp/src/RVL_SDK/gx/$u.c" -o "$out/gx/$u.o"; }
    objs+=("$out/gx/$u.o")
  done
  [[ -f "$out/gx/mtx.o" ]] || nice "$CC" -std=gnu11 -O1 -c "${GINC[@]}" $CW "$root/platform/src/sdk/mtx.c" -o "$out/gx/mtx.o"
  objs+=("$out/gx/mtx.o")
  [[ -f "$out/gx/stub.o" ]] || nice "$CC" -std=gnu11 -O1 -c "${GINC[@]}" $CW "$root/tests/bloom_fast_gl_gx.c" -o "$out/gx/stub.o"
  [[ -f "$out/gx/jma.o" ]] || nice "$CXX" -std=gnu++20 -O1 -c "${GINC[@]}" $W "$root/decomp/src/JSystem/JMath/JMATrigonometric.cpp" -o "$out/gx/jma.o"
  [[ -f "$out/gx/cfn.o" ]] || nice "$CXX" -std=gnu++20 -O1 -c "${GINC[@]}" $W "$root/decomp/src/Game/Screen/CopyFilterNegater.cpp" -o "$out/gx/cfn.o"
  [[ -f "$out/gx/cap.o" ]] || nice "$CXX" -std=gnu++20 -O1 -c "${GINC[@]}" $W "$root/platform/src/gx/gx_capture.cpp" -o "$out/gx/cap.o"
  [[ -f "$out/gx/stubs2.o" ]] || nice "$CC" -std=gnu11 -O1 -c "${GINC[@]}" $CW "$root/tests/clipblur_gl_stubs.c" -o "$out/gx/stubs2.o"
  objs+=("$out/gx/stubs2.o" "$out/gx/stub.o" "$out/gx/jma.o" "$out/gx/cfn.o" "$out/gx/cap.o")
  nice "$CXX" -std=gnu++20 -O1 -c "${GINC[@]}" $W -I"$root/decomp/include" "$tree/decomp/src/Game/MapObj/FallOutFieldDraw.cpp" -o "$o/bloom.o"
  nice "$CXX" -std=gnu++20 -O1 -c "${GINC[@]}" $W "$tree/decomp/src/Game/Screen/ImageEffectLocalUtil.cpp" -o "$o/ieu.o"
  : > "$o/simple.c"; nice "$CC" -c "$o/simple.c" -o "$o/simple.o"
  nice "$CXX" -std=gnu++20 -O1 -c "${GINC[@]}" $W "$root/tests/clipblur_gl_game.cpp" $([[ $tree == "$root" ]] || echo -DCB_BASE=1) -o "$o/game.o"
  mkdir -p "$out/vgx"
  for u in vgx_state vgx_draw vgx_fesplit vgx_dlcache vgx_guestmem vgx_bpsync vgx_shadergen vgx_texcache vgx_texdecode vgx_stats vgx_backend_gl vgx_autoq vgx_texcomp; do
    [[ -f "$src/$u.cpp" ]] || continue  # (vgx_texcomp: from texcomp23 on)
    [[ -f "$out/vgx/$u.o" ]] || nice g++ -std=c++20 -O1 -g0 -fno-pie -I"$src" -I"$root/platform/include" -I"$root/platform/vgx/third_party" -I"$khr" -c "$src/$u.cpp" -o "$out/vgx/$u.o"
    objs+=("$out/vgx/$u.o")
  done
  [[ -f "$out/vgx/host.o" ]] || nice g++ -std=c++20 -O1 -fno-pie -I"$src" -I"$root/platform/include" -I"$root/platform/vgx/third_party" -I"$khr" -c "$root/platform/vgx/host/vgx_host_egl.cpp" -o "$out/vgx/host.o"
  nice g++ -std=c++20 -O1 -fno-pie -I"$src" -I"$root/platform/include" -I"$root/platform/vgx/third_party" -I"$khr" -c "$root/tests/clipblur_gl_main.cpp" -o "$o/main.o"
  g++ -no-pie "$o/main.o" "$o/game.o" "$o/bloom.o" "$o/ieu.o" "$o/simple.o" "${objs[@]}" "$out/vgx/host.o" -Wl,--gc-sections -Wl,--allow-multiple-definition -lEGL -lGLESv2 -pthread -lm -o "$exe"
}
# The reference: BloomEffect.cpp / ImageEffectLocalUtil.cpp of CB_BASE (a tree), else of git revision CB_BASE_REV (default 7c5feea).
if [[ -z "${CB_BASE:-}" ]]; then
  CB_BASE="$out/basetree"; mkdir -p "$CB_BASE/decomp/src/Game/Screen" "$CB_BASE/decomp/src/Game/MapObj"
  for f in ImageEffectLocalUtil; do
    git -C "$root" show "${CB_BASE_REV:-7c5feea}:decomp/src/Game/Screen/$f.cpp" > "$CB_BASE/decomp/src/Game/Screen/$f.cpp"
  done
  git -C "$root" show "${CB_BASE_REV:-7c5feea}:decomp/src/Game/MapObj/FallOutFieldDraw.cpp" > "$CB_BASE/decomp/src/Game/MapObj/FallOutFieldDraw.cpp"
fi
build "$root" "$out/new" "$out/clipblur_gl"
build "$CB_BASE" "$out/base" "$out/clipblur_base_gl"
echo "built $out"
[[ "${CB_BUILD_ONLY:-0}" == 1 ]] && exit 0

