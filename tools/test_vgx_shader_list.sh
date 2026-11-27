#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
image=codex-galaxy-native:20261007-arm64
compiler=clang++
flags='-std=c++17 -stdlib=libc++ -O1 -g -DVGX_NO_NEON -D__VGX_VITA__ -fsanitize=address,undefined -fno-sanitize-link-runtime -fno-omit-frame-pointer'
libs='-l:libasan.so.8 -l:libubsan.so.1'
run=(env)
if [[ ${1:-} == arm32 ]]; then
  image=codex-strikers-arm-tests:20261006
  compiler=arm-linux-gnueabihf-g++
  flags='-std=c++17 -O2 -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -DVGX_NO_NEON -D__VGX_VITA__ -Wno-psabi'
  libs=''
  run=(qemu-arm -cpu cortex-a9 -L /usr/arm-linux-gnueabihf)
fi
# host-fallback: tests31: with no usable docker daemon (and for arm64 hosts, which have no 64-bit image here) the same body runs on THIS host: nothing in it needs the image,
# only a C++17 compiler; it uses tools/host_san.sh (clang++ with sanitizers, else g++ with them, else plain, with a note). The arm32 leg still needs docker + qemu.
body='
    export ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1
    for seed in 0 1; do
      $CXX $FLAGS -DGXV_RECIPE_SEED_BATCH=$seed -Iplatform/include -Iplatform/vgx/src -Iplatform/vgx/third_party \
        tests/vgx_shader_list.cpp platform/vgx/src/vgx_state.cpp platform/vgx/src/vgx_draw.cpp platform/vgx/src/vgx_fesplit.cpp \
        platform/vgx/src/vgx_dlcache.cpp platform/vgx/src/vgx_guestmem.cpp platform/vgx/src/vgx_bpsync.cpp \
        platform/vgx/src/vgx_shadergen.cpp platform/vgx/src/vgx_texcache.cpp platform/vgx/src/vgx_texcomp.cpp platform/vgx/src/vgx_texdecode.cpp \
        platform/vgx/src/vgx_stats.cpp platform/vgx/src/vgx_backend_null.cpp -o "${RECIPE_BIN:-/tmp/recipe}" $LIBS
      for mode in clean duplicate tail-1 tail-2 tail-3 payload alpha-zero alpha-zero-duplicate bad-program bad-variant; do
        for deferred in 0 1; do timeout 30s "$@" "${RECIPE_BIN:-/tmp/recipe}" "$mode" "$deferred"; done
      done
    done
'
if docker info >/dev/null 2>&1; then
  docker run --rm --pull=never --platform linux/arm64 --network none --cpus=2 \
    --mount "type=bind,source=$root,target=/work,readonly" -w /work \
    -e "CXX=$compiler" -e "FLAGS=$flags" -e "LIBS=$libs" \
    "$image" bash -euo pipefail -c "$body" -- "${run[@]}"
else
  if [[ ${1:-} == arm32 ]]; then echo "needs docker + qemu-arm for the arm32 leg"; exit 77; fi
  . "$root/tools/host_san.sh"
  cxx=$(host_pick_cxx)
  san=($(host_san "$cxx" -fsanitize=address,undefined -fno-omit-frame-pointer))
  echo "vgx_shader_list: no docker daemon: running on this host with $cxx ${san[*]:-(NO SANITIZERS)}"
  cd "$root"
  tmpd=$(mktemp -d); trap 'rm -rf "$tmpd"' EXIT
  RECIPE_BIN=$tmpd/recipe CXX=$cxx FLAGS="-std=c++17 -O1 -g -DVGX_NO_NEON -D__VGX_VITA__ ${san[*]-}" LIBS= ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 \
    bash -euo pipefail -c "$body" -- env
fi
