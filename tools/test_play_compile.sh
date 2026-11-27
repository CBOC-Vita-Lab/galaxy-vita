#!/usr/bin/env bash
# noprog26 (GXV_NO_PLAY_COMPILE_STOP, GXV_STAGE_PRECOMPILE): host test of vgx_play_compile.h and the gate's missOnly mode (plain and ASan+UBSan), then the back end's use of them on the GL stub
# (tests/vgx_play_compile_backend.cpp, no worker), then nohitch26's backend test again (modes 0-2: unchanged with the new switch off).
#   bash tools/test_play_compile.sh [out-dir]
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d "${TMPDIR:-/tmp}/playcompile.XXXXXX")}
mkdir -p "$out"
# (tests31: the model builds with any compiler, plain and then sanitized when host_san says the runtime works; the back end (vgx_backend_gl.cpp) builds
#  with clang++ only: g++ 13 refuses its two extern "C" port_scene_serial declarations as "overloaded function"; BACKEND_CXX overrides)
. "$root/tools/host_san.sh"
mcxx=${CXX:-g++}; bcxx=$(host_backend_cxx)
san=($(host_san "$mcxx" -fsanitize=address,undefined -fno-sanitize-recover=undefined))
for flags in "" ${san[@]+"${san[*]}"}; do
    "$mcxx" -std=gnu++17 -O1 -g -Wall -Wextra $flags "$root/tests/play_compile_model.cpp" -o "$out/play_compile_model"
    "$out/play_compile_model"
done
FLAGS=(-std=c++17 -O1 -DNDEBUG -fpermissive -w -D__VGX_VITA__=1 -include "$root/tests/play_compile_glshim.h" -I"$root/armhf/glstub" -I"$root/tests/autoq_clock_stubs" -I"$root/platform/include"
  -I"$root/platform/vgx/src" -I"$root/platform/vgx/third_party")
SRCS=("$root/platform/vgx/src/vgx_shadergen.cpp" "$root/platform/vgx/src/vgx_autoq.cpp")
"$bcxx" "${FLAGS[@]}" "$root/tests/vgx_play_compile_backend.cpp" "${SRCS[@]}" -o "$out/vgx_play_compile_backend" -lpthread
"$out/vgx_play_compile_backend"
"$bcxx" "${FLAGS[@]}" "$root/tests/vgx_play_compile_worker.cpp" "${SRCS[@]}" -o "$out/vgx_play_compile_worker" -lpthread
"$out/vgx_play_compile_worker"
"$bcxx" "${FLAGS[@]}" "$root/tests/vgx_async_link_backend.cpp" "${SRCS[@]}" -o "$out/vgx_async_link_backend" -lpthread
for m in 0 1 2; do "$out/vgx_async_link_backend" $m; done
GXV_MISS_STANDIN=0 "$out/vgx_async_link_backend" 1   # (tests31: hitch33's default stands the draws in; 0 is nohitch26's skip)
# revA: review of the 12 Nov patches: failed worker jobs go to the gate, a busy worker is aged out, occ/stencil draws are never held
"$bcxx" "${FLAGS[@]}" "$root/tests/vgx_review_a_worker.cpp" "${SRCS[@]}" -o "$out/vgx_review_a_worker" -lpthread
"$out/vgx_review_a_worker"
