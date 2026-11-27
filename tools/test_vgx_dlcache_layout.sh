#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
image=codex-galaxy-native:20261007-arm64
compiler=clang++
flags='-std=c++17 -stdlib=libc++ -O1 -g -DVGX_NO_NEON -fsanitize=address,undefined -fno-sanitize-link-runtime -fno-omit-frame-pointer'
libs='-l:libasan.so.8 -l:libubsan.so.1'
run=(env)
if [[ ${1:-} == arm32 ]]; then
  image=codex-strikers-arm-tests:20261006
  compiler=arm-linux-gnueabihf-g++
  flags='-std=c++17 -O2 -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -Wno-psabi'
  libs=''
  run=(qemu-arm -cpu cortex-a9 -L /usr/arm-linux-gnueabihf)
fi
docker run --rm --pull=never --platform linux/arm64 --network none --cpus=2 \
  --mount "type=bind,source=$root,target=/work,readonly" -w /work \
  -e "CXX=$compiler" -e "FLAGS=$flags" -e "LIBS=$libs" \
  "$image" bash -euo pipefail -c '
    export ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1
    cases=0
    for diagnostic in 0 1; do
      $CXX $FLAGS -DPORT_RUNTIME_DIAGNOSTICS=$diagnostic -DPORT_LOGGING=0 \
        -Iplatform/vgx/src -Iplatform/vgx/third_party \
        tests/vgx_dlcache_layout.cpp platform/vgx/src/vgx_state.cpp platform/vgx/src/vgx_draw.cpp platform/vgx/src/vgx_fesplit.cpp \
        platform/vgx/src/vgx_guestmem.cpp platform/vgx/src/vgx_bpsync.cpp platform/vgx/src/vgx_shadergen.cpp \
        platform/vgx/src/vgx_texcache.cpp platform/vgx/src/vgx_texdecode.cpp platform/vgx/src/vgx_stats.cpp \
        platform/vgx/src/vgx_backend_null.cpp -o /tmp/cache-layout $LIBS
      for indices in 0 1; do for merge in 0 1; do for mode in dynamic static; do
        GXV_ABSIDX=$indices GXV_NOMERGE=$merge timeout 90s "$@" /tmp/cache-layout "$mode"
        cases=$((cases+1))
      done; done; done
    done
    printf "PASS: %u DL-cache layout scenarios across twelve layouts and eight vertex formats\n" "$cases"
  ' -- "${run[@]}"
