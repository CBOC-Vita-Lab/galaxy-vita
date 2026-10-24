#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
fixture=$(mktemp -d "$root/local/cache-ordering.XXXXXX")
trap 'rm -rf "$fixture"' EXIT
node "$root/tools/prepare_vgx_cache_ordering.cjs" "$fixture"
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
elif [[ ${1:-} == tsan ]]; then
  flags='-std=c++17 -stdlib=libc++ -O1 -g -DVGX_NO_NEON -fsanitize=thread -fno-sanitize-link-runtime -fno-omit-frame-pointer'
  libs='-l:libtsan.so.2'
fi
docker run --rm --pull=never --platform linux/arm64 --network none --cpus=2 \
  --mount "type=bind,source=$root,target=/work,readonly" \
  --mount "type=bind,source=$fixture,target=/fixture,readonly" -w /work \
  -e "CXX=$compiler" -e "FLAGS=$flags" -e "LIBS=$libs" \
  -e "ORDERING_CASES=${ORDERING_CASES:-}" -e "ORDERING_QUIET=${ORDERING_QUIET:-}" \
  "$image" bash -euo pipefail -c '
    export ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 TSAN_OPTIONS=halt_on_error=1
    cases=0
    failures=0
    for diagnostic in ${ORDERING_QUIET:-0 1}; do
      $CXX $FLAGS -pthread -DPORT_RUNTIME_DIAGNOSTICS=$diagnostic -DPORT_LOGGING=0 \
        -Iplatform/vgx/src -Iplatform/vgx/third_party \
        tests/vgx_cache_ordering.cpp /fixture/guest.cpp /fixture/dl.cpp \
        platform/vgx/src/vgx_state.cpp platform/vgx/src/vgx_draw.cpp platform/vgx/src/vgx_fesplit.cpp platform/vgx/src/vgx_bpsync.cpp \
        platform/vgx/src/vgx_shadergen.cpp /fixture/texture.cpp platform/vgx/src/vgx_texdecode.cpp \
        platform/vgx/src/vgx_stats.cpp platform/vgx/src/vgx_backend_null.cpp -o /tmp/cache-ordering $LIBS
      $CXX $FLAGS -pthread -Iplatform/vgx/src tests/vgx_guestmem_concurrent.cpp \
        platform/vgx/src/vgx_guestmem.cpp -o /tmp/guestmem-concurrent $LIBS
      timeout 90s "$@" /tmp/guestmem-concurrent
      cases=$((cases+1))
      modes=(${ORDERING_CASES:-dl-after dl-mixed dl-preclaim dl-before dl-disjoint dl-same-page dl-completed-noise dl-overflow texture-before texture-after texture-log texture-aba ring lapped stress spans})
      if test -z "$ORDERING_CASES"; then
        for stage in 1 2 3 4; do
          modes+=("dl-build-$stage" "dl-build-$stage-complete" "dl-list-build-$stage" "dl-list-build-$stage-complete")
        done
      fi
      for cache in 1 verify; do for indices in 0 1; do
        for mode in "${modes[@]}"; do
          if ! GXV_DLCACHE=$cache GXV_ABSIDX=$indices timeout 90s "$@" /tmp/cache-ordering "$mode"; then
            failures=$((failures+1))
          fi
          cases=$((cases+1))
        done
      done; done
    done
    test "$failures" = 0
    printf "PASS: %u cache ordering scenarios (synthetic scheduling, not hardware FPS)\n" "$cases"
  ' -- "${run[@]}"
