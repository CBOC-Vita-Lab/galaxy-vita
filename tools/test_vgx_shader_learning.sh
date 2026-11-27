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
docker run --rm --pull=never --platform linux/arm64 --network none --cpus=2 \
  --mount "type=bind,source=$root,target=/work,readonly" -w /work \
  -e "CXX=$compiler" -e "FLAGS=$flags" -e "LIBS=$libs" \
  -e "ONLY_LEARNING_CASE=${2:-}" \
  -e "LEARNING_EXHAUSTIVE=${LEARNING_EXHAUSTIVE:-0}" \
  "$image" bash -euo pipefail -c '
    export ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1
    for seed in 0 1; do
      $CXX $FLAGS -DGXV_RECIPE_SEED_BATCH=$seed -Wl,--wrap=fopen,--wrap=fopen64,--wrap=fclose,--wrap=fwrite,--wrap=fflush \
        -Iplatform/vgx/src -Iplatform/vgx/third_party tests/vgx_shader_learning.cpp platform/vgx/src/vgx_fesplit.cpp \
        platform/vgx/src/vgx_state.cpp platform/vgx/src/vgx_dlcache.cpp platform/vgx/src/vgx_guestmem.cpp \
        platform/vgx/src/vgx_bpsync.cpp platform/vgx/src/vgx_shadergen.cpp platform/vgx/src/vgx_texcache.cpp \
        platform/vgx/src/vgx_texdecode.cpp platform/vgx/src/vgx_stats.cpp platform/vgx/src/vgx_backend_null.cpp \
        -o /tmp/learning $LIBS
      if test -n "$ONLY_LEARNING_CASE"; then
        timeout 20s "$@" /tmp/learning "$ONLY_LEARNING_CASE" /tmp/learning.bin
        exit
      fi
      timeout 20s "$@" /tmp/learning cold /tmp/learning.bin
      timeout 20s "$@" /tmp/learning warm /tmp/learning.bin
      cp /tmp/learning.bin /tmp/learning-complete.bin
      timeout 20s "$@" /tmp/learning burst /tmp/learning.bin
      for mode in write-fail-1 write-fail-2 write-fail-3 write-fail-7 write-fail-23 write-fail-500 flush-fail; do
        timeout 20s "$@" /tmp/learning "$mode" /tmp/learning.bin
        timeout 20s "$@" /tmp/learning recover /tmp/learning.bin
      done
      timeout 20s "$@" /tmp/learning rewrite-fail /tmp/learning.bin
      timeout 20s "$@" /tmp/learning read-fail /tmp/learning.bin
      timeout 20s "$@" /tmp/learning read-open-fail /tmp/learning.bin
      timeout 20s "$@" /tmp/learning no-list /tmp/learning.bin
      timeout 20s "$@" /tmp/learning unwritable /tmp/nonexistent-learning-parent/file.bin
      timeout 20s "$@" /tmp/learning device-full /dev/full
      timeout 20s "$@" /tmp/learning reset /tmp/learning.bin
      timeout 20s "$@" /tmp/learning path-change /tmp/learning.bin
      if test "$seed" = 1; then
        timeout 20s "$@" /tmp/learning checked /tmp/learning.bin
        timeout 20s "$@" /tmp/learning warm /tmp/learning.bin
        timeout 20s "$@" /tmp/learning checked-fail /tmp/learning.bin
        timeout 20s "$@" /tmp/learning recover /tmp/learning.bin
      fi
      if test "$LEARNING_EXHAUSTIVE" = 1; then
        bytes=$(stat -c %s /tmp/learning-complete.bin)
        cuts=0
        for cut in $(seq 24 $((bytes - 1))); do
          head -c "$cut" /tmp/learning-complete.bin > /tmp/learning.bin
          if ! timeout 20s "$@" /tmp/learning recover /tmp/learning.bin > /tmp/cut-result 2>&1; then
            cat /tmp/cut-result
            exit 1
          fi
          cuts=$((cuts + 1))
        done
        printf "PASS: %u exact-byte truncation/recovery cases; recipe flag=%s\n" "$cuts" "$seed"
      fi
    done
  ' -- "${run[@]}"
