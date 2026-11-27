#!/usr/bin/env bash
# Builds tests/lightvol_gl_{game,main}.cpp with the PRODUCTION decomp/src/Game/LiveActor/VolumeModelDrawer.cpp, the console's GX library and the vgx front end + GLES back
# end (as tools/test_clipblur_gl.sh). tools/lightvol_build.sh <out-dir>   env: KHR_INCLUDE
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=$1
mkdir -p "$out/gx" "$out/vgx"
khr=${KHR_INCLUDE:-/work/khr/include}
src="$root/platform/vgx/src"
W="-w -Wno-register"
CW="-w -Wno-implicit-function-declaration -Wno-int-conversion -Wno-incompatible-pointer-types -Wno-incompatible-function-pointer-types"
GINC=(-DNDEBUG=1 -DTARGET_PC=1 -DVERSION=0 -I"$root/vita/include" -I"$root/platform/include" -I"$root/decomp/include" -I"$root/decomp/libs/JSystem/include"
      -I"$root/decomp/libs/RVL_SDK/include" -I"$root/decomp/libs/nw4r/include" -fsigned-char -fno-strict-aliasing -fwrapv -fms-extensions -ffunction-sections
      -fdata-sections -include "$root/vita/include/vita_source_prelude.h" -include "$root/platform/include/port/compat.h" -fno-pie)
CC=${CC:-clang}; CXX=${CXX:-clang++}
objs=()
for u in GXAttr GXBump GXDisplayList GXFrameBuf GXGeometry GXInit GXLight GXMisc GXPixel GXTev GXTexture GXTransform; do
  [[ -f "$out/gx/$u.o" ]] || nice "$CC" -std=gnu11 -O1 -c "${GINC[@]}" $CW "$root/decomp/src/RVL_SDK/gx/$u.c" -o "$out/gx/$u.o"
  objs+=("$out/gx/$u.o")
done
[[ -f "$out/gx/mtx.o" ]] || nice "$CC" -std=gnu11 -O1 -c "${GINC[@]}" $CW "$root/platform/src/sdk/mtx.c" -o "$out/gx/mtx.o"
[[ -f "$out/gx/stub.o" ]] || nice "$CC" -std=gnu11 -O1 -c "${GINC[@]}" $CW "$root/tests/bloom_fast_gl_gx.c" -o "$out/gx/stub.o"
[[ -f "$out/gx/jma.o" ]] || nice "$CXX" -std=gnu++20 -O1 -c "${GINC[@]}" $W "$root/decomp/src/JSystem/JMath/JMATrigonometric.cpp" -o "$out/gx/jma.o"
[[ -f "$out/gx/cap.o" ]] || nice "$CXX" -std=gnu++20 -O1 -c "${GINC[@]}" $W "$root/platform/src/gx/gx_capture.cpp" -o "$out/gx/cap.o"
[[ -f "$out/gx/stubs2.o" ]] || nice "$CC" -std=gnu11 -O1 -c "${GINC[@]}" $CW "$root/tests/clipblur_gl_stubs.c" -o "$out/gx/stubs2.o"
objs+=("$out/gx/mtx.o" "$out/gx/stub.o" "$out/gx/jma.o" "$out/gx/cap.o" "$out/gx/stubs2.o")
nice "$CXX" -std=gnu++20 -O1 -c "${GINC[@]}" $W "$root/decomp/src/Game/LiveActor/VolumeModelDrawer.cpp" -o "$out/vmd.o"
nice "$CXX" -std=gnu++20 -O1 -c "${GINC[@]}" $W "$root/tests/lightvol_gl_game.cpp" -o "$out/game.o"
for u in vgx_state vgx_draw vgx_fesplit vgx_dlcache vgx_guestmem vgx_bpsync vgx_shadergen vgx_texcache vgx_texdecode vgx_stats vgx_backend_gl vgx_autoq vgx_texcomp; do
  [[ -f "$src/$u.cpp" ]] || continue
  [[ -f "$out/vgx/$u.o" ]] || nice g++ -std=c++20 -O1 -g0 -fno-pie -I"$src" -I"$root/platform/include" -I"$root/platform/vgx/third_party" -I"$khr" -c "$src/$u.cpp" -o "$out/vgx/$u.o"
  objs+=("$out/vgx/$u.o")
done
[[ -f "$out/vgx/host.o" ]] || nice g++ -std=c++20 -O1 -fno-pie -I"$src" -I"$root/platform/include" -I"$root/platform/vgx/third_party" -I"$khr" -c "$root/platform/vgx/host/vgx_host_egl.cpp" -o "$out/vgx/host.o"
nice g++ -std=c++20 -O1 -fno-pie -I"$src" -I"$root/platform/include" -I"$root/platform/vgx/third_party" -I"$khr" -c "$root/tests/lightvol_gl_main.cpp" -o "$out/main.o"
g++ -no-pie "$out/main.o" "$out/game.o" "$out/vmd.o" "${objs[@]}" "$out/vgx/host.o" -Wl,--gc-sections -Wl,--allow-multiple-definition -lEGL -lGLESv2 -pthread -lm -o "$out/lightvol_gl"
echo "built $out/lightvol_gl"
