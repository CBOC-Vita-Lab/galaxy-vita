#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
image=codex-galaxy-native:20261007-arm64
compiler=clang++
flags='-std=c++17 -stdlib=libc++ -O1 -g -fsanitize=address,undefined -fno-sanitize-link-runtime -fno-omit-frame-pointer'
libs='-l:libasan.so.8 -l:libubsan.so.1'
run=(env)
if [[ ${3:-} == arm32 ]]; then
  image=codex-strikers-arm-tests:20261006
  compiler=arm-linux-gnueabihf-g++
  flags='-std=c++17 -O2 -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -Wno-psabi'
  libs=''
  run=(qemu-arm -cpu cortex-a9 -L /usr/arm-linux-gnueabihf)
fi
docker run --rm --pull=never --platform linux/arm64 --network none --cpus=2 \
  --mount "type=bind,source=$root,target=/work,readonly" -w /work \
  -e "ONLY_FRONTEND_CONFIG=${1:-}" -e "ONLY_FRONTEND_CASE=${2:-}" \
  -e "CXX=$compiler" -e "FLAGS=$flags" -e "LIBS=$libs" \
  "$image" bash -euo pipefail -c '
    export ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1
    cases=0
    failed=0
    for config in default on quiet quiet-logging; do
      if test -n "$ONLY_FRONTEND_CONFIG" && test "$config" != "$ONLY_FRONTEND_CONFIG"; then continue; fi
      flags=(-DTEST_DIAGNOSTICS=1 -DPORT_LOGGING=0)
      if test "$config" = on; then flags+=(-DPORT_RUNTIME_DIAGNOSTICS=1); fi
      if test "$config" = quiet || test "$config" = quiet-logging; then
        flags=(-DTEST_DIAGNOSTICS=0 -DPORT_RUNTIME_DIAGNOSTICS=0 -DPORT_LOGGING=0)
      fi
      if test "$config" = quiet-logging; then flags+=(-UPORT_LOGGING -DPORT_LOGGING=1); fi
      $CXX $FLAGS -D__VGX_VITA__ \
        "${flags[@]}" -Wl,--wrap=getenv -Iplatform/vgx/src -Iplatform/vgx/third_party \
        tests/vgx_frontend_quiet.cpp platform/vgx/src/vgx_fesplit.cpp platform/vgx/src/vgx_state.cpp platform/vgx/src/vgx_dlcache.cpp \
        platform/vgx/src/vgx_guestmem.cpp platform/vgx/src/vgx_bpsync.cpp platform/vgx/src/vgx_shadergen.cpp \
        platform/vgx/src/vgx_texcache.cpp platform/vgx/src/vgx_texdecode.cpp platform/vgx/src/vgx_backend_null.cpp \
        -o /tmp/frontend-quiet $LIBS
      for mode in heap-counter stats files files-ram passcheck repeat light light-off trace front-baseline front-full front decode decode-plain fold fold-off soft soft-scalar resolve-base fold-stream-base resolve fold-stream; do
        if test -n "$ONLY_FRONTEND_CASE" && test "$mode" != "$ONLY_FRONTEND_CASE" && test "$mode" != "$ONLY_FRONTEND_CASE-base"; then continue; fi
        if ! timeout 20s "$@" /tmp/frontend-quiet "$mode" > /tmp/frontend-out; then
          cat /tmp/frontend-out
          failed=$((failed + 1))
          continue
        fi
        cat /tmp/frontend-out
        if test "$mode" = front || test "$mode" = front-baseline || test "$mode" = front-full; then
          actual=$(sed -n "s/^geometry=//p" /tmp/frontend-out)
          test -n "$actual"
          if test -f /tmp/frontend-geometry; then test "$actual" = "$(cat /tmp/frontend-geometry)"; else printf "%s" "$actual" > /tmp/frontend-geometry; fi
        fi
        if test "$mode" = resolve-base || test "$mode" = fold-stream-base; then
          sed -n "s/.*allocations=\([0-9]*\).*/\1/p" /tmp/frontend-out > "/tmp/$mode-allocations"
        elif test "$mode" = resolve || test "$mode" = fold-stream; then
          actual=$(sed -n "s/.*allocations=\([0-9]*\).*/\1/p" /tmp/frontend-out)
          baseline=$(cat "/tmp/$mode-base-allocations")
          if test "$config" = quiet || test "$config" = quiet-logging; then
            if test "$actual" != "$baseline"; then
              printf "FAIL: quiet mixed resolve/fold selectors skip duplicate-state and vertex copies\n"
              failed=$((failed + 1))
            fi
          elif ! test "$actual" -gt "$baseline"; then
            printf "FAIL: diagnostic mixed selectors execute their extra reference copies\n"
            failed=$((failed + 1))
          fi
        fi
        cases=$((cases + 1))
      done
    done
    test "$failed" = 0
    printf "PASS: %u real frontend quiet scenarios; unchanged synthetic geometry, not Vita FPS\n" "$cases"
  ' -- "${run[@]}"
