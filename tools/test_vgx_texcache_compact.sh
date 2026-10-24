#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
image=codex-galaxy-native:20261007-arm64
compiler=clang++
flags='-std=c++17 -stdlib=libc++ -O1 -g -DVGX_NO_NEON -fsanitize=address,undefined -fno-sanitize-link-runtime -fno-omit-frame-pointer'
libs='-l:libasan.so.8 -l:libubsan.so.1'
run=(env)
if [[ ${2:-} == arm32 ]]; then
  image=codex-strikers-arm-tests:20261006
  compiler=arm-linux-gnueabihf-g++
  flags='-std=c++17 -O2 -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -DVGX_NO_NEON -Wno-psabi'
  libs=''
  run=(qemu-arm -cpu cortex-a9 -L /usr/arm-linux-gnueabihf)
fi
docker run --rm --pull=never --platform linux/arm64 --network none --cpus=2 \
  --mount "type=bind,source=$root,target=/work,readonly" -w /work \
  -e "COMPACT_MODES=${1:-plain legacy accept reject mip switch}" \
  -e "CXX=$compiler" -e "FLAGS=$flags" -e "LIBS=$libs" \
  "$image" bash -euo pipefail -c '
    export ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1
    for scalar in native scalar; do
      extra=()
      if [[ $scalar == scalar ]];then extra=(-U__ARM_NEON -U__ARM_NEON__);fi
      $CXX $FLAGS "${extra[@]}" -Iplatform/vgx/src tests/vgx_texdecode_compact.cpp platform/vgx/src/vgx_texdecode.cpp -o /tmp/decode $LIBS
      timeout 30s "$@" /tmp/decode
    done
    $CXX $FLAGS \
      -Iplatform/vgx/src -Iplatform/vgx/third_party tests/vgx_texcache_compact.cpp \
      platform/vgx/src/vgx_state.cpp platform/vgx/src/vgx_draw.cpp platform/vgx/src/vgx_fesplit.cpp platform/vgx/src/vgx_dlcache.cpp \
      platform/vgx/src/vgx_guestmem.cpp platform/vgx/src/vgx_bpsync.cpp platform/vgx/src/vgx_shadergen.cpp \
      platform/vgx/src/vgx_texcache.cpp platform/vgx/src/vgx_texdecode.cpp platform/vgx/src/vgx_stats.cpp \
      platform/vgx/src/vgx_backend_null.cpp -o /tmp/compact $LIBS
    for la in 1 0; do for mode in $COMPACT_MODES; do
      GXV_LA88=$la timeout 20s "$@" /tmp/compact "$mode" "$la"
    done; done
  ' -- "${run[@]}"
