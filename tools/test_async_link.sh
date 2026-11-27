#!/usr/bin/env bash
# nohitch26 (GXV_ASYNC_LINK): host test of vgx_async_link.h, plain and ASan+UBSan, then the back end's use of it on the GL stub (tests/vgx_async_link_backend.cpp) in modes 0, 1 and 2.
#   bash tools/test_async_link.sh [out-dir]
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d "${TMPDIR:-/tmp}/asynclink.XXXXXX")}
mkdir -p "$out"
. "$root/tools/host_san.sh"   # (tests31: the model builds plain and then sanitized when the runtime works; the back end (vgx_backend_gl.cpp) needs clang++: see host_backend_cxx)
mcxx=${CXX:-g++}; bcxx=$(host_backend_cxx)
san=($(host_san "$mcxx" -fsanitize=address,undefined -fno-sanitize-recover=undefined))
for flags in "" ${san[@]+"${san[*]}"}; do
    "$mcxx" -std=gnu++17 -O1 -g -Wall -Wextra $flags "$root/tests/async_link_model.cpp" -o "$out/async_link_model"
    "$out/async_link_model"
done
"$bcxx" -std=c++17 -O1 -DNDEBUG -fpermissive -w -D__VGX_VITA__=1 -include "$root/tests/play_compile_glshim.h" -I"$root/armhf/glstub" -I"$root/tests/autoq_clock_stubs" -I"$root/platform/include" \
  -I"$root/platform/vgx/src" -I"$root/platform/vgx/third_party" "$root/tests/vgx_async_link_backend.cpp" "$root/platform/vgx/src/vgx_shadergen.cpp" \
  "$root/platform/vgx/src/vgx_autoq.cpp" -o "$out/vgx_async_link_backend" -lpthread
for m in 0 1 2; do "$out/vgx_async_link_backend" $m; done
GXV_MISS_STANDIN=0 "$out/vgx_async_link_backend" 1   # (tests31: hitch33's default stands the draws in; 0 is nohitch26's skip)
