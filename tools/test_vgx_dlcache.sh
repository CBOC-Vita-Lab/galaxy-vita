#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
image=codex-galaxy-native:20261007-arm64
test "$(docker image inspect --format '{{.Architecture}}' "$image")" = arm64
docker run --rm --pull=never --platform linux/arm64 --network none --cpus=2 \
  -v "$root:/work:ro" -w /work -e "QUIET_CACHE_TEST=${1:-}" "$image" bash -euo pipefail -c '
    test "$(uname -m)" = aarch64
    flags=()
    if test "$QUIET_CACHE_TEST" = quiet; then flags+=(-DPORT_RUNTIME_DIAGNOSTICS=0 -DPORT_LOGGING=0); fi
    clang++ -std=c++17 -stdlib=libc++ -O1 -g -DVGX_NO_NEON "${flags[@]}" \
      -fsanitize=address,undefined -fno-sanitize-link-runtime -fno-omit-frame-pointer \
      -Iplatform/vgx/src -Iplatform/vgx/third_party tests/vgx_dlcache.cpp \
      platform/vgx/src/vgx_state.cpp platform/vgx/src/vgx_draw.cpp platform/vgx/src/vgx_fesplit.cpp \
      platform/vgx/src/vgx_dlcache.cpp platform/vgx/src/vgx_guestmem.cpp \
      platform/vgx/src/vgx_bpsync.cpp platform/vgx/src/vgx_shadergen.cpp \
      platform/vgx/src/vgx_texcache.cpp platform/vgx/src/vgx_texdecode.cpp \
      platform/vgx/src/vgx_stats.cpp platform/vgx/src/vgx_backend_null.cpp \
      -o /tmp/vgx_dlcache -l:libasan.so.8 -l:libubsan.so.1
    # Cache entries intentionally live until process exit; this checks access/UB, not teardown leaks.
    export ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1
    cases=0
    for indices in 0 1; do
      for cache in 1 verify; do
        for hash in 0 2; do
          for mode in clean other-page warm-then-noise list-page array-page same-list-flush change-once change-each small-list static-disabled age-out alternate-edit change-then-noise alternate-clean periodic-3 periodic-4; do
            GXV_ABSIDX=$indices GXV_DLCACHE=$cache GXV_HASH=$hash timeout 10s /tmp/vgx_dlcache "$mode"
            cases=$((cases + 1))
          done
        done
      done
    done
    printf "PASS: %s production display-list cache scenarios (synthetic geometry, not Vita FPS)\n" "$cases"
  '
