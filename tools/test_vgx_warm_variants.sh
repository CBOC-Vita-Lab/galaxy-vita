#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
docker run --rm --pull=never --platform linux/arm64 --network none --cpus=2 \
  --mount "type=bind,source=$root,target=/work,readonly" -w /work \
  -e "WARM_CASE=${1:-}" -e EGL_PLATFORM=surfaceless -e LIBGL_ALWAYS_SOFTWARE=1 -e GALLIUM_DRIVER=softpipe \
  codex-galaxy-native:20261007-arm64 bash -euo pipefail -c '
    for diagnostics in 0 1; do
    for probe in 0 1; do
    clang++ -std=c++17 -stdlib=libc++ -O1 -g -DVGX_NO_NEON -D__VGX_VITA__ -DVGX_COLDTRACE=$probe -DPORT_RUNTIME_DIAGNOSTICS=$diagnostics \
      -ffunction-sections -fdata-sections -Wl,--gc-sections -Iplatform/vgx/src -Iplatform/vgx/third_party \
      -fsanitize=address,undefined -fno-sanitize-link-runtime -fno-omit-frame-pointer \
      tests/vgx_warm_variants.cpp platform/vgx/src/vgx_shadergen.cpp -o /tmp/warm-variants -Wl,--wrap=glDrawElements -lEGL -lGLESv2 -pthread \
      -l:libasan.so.8 -l:libubsan.so.1
    export LD_PRELOAD=libasan.so.8 ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1
    if test -n "$WARM_CASE"; then timeout 30s /tmp/warm-variants "$WARM_CASE" 1; continue; fi
    for dedup in 0 1; do
      for mode in repeat real pass pass-after-warm switched layout blend ids evict empty clipped failed offset queue execute lines points real-blend queue-shaders; do
        timeout 30s /tmp/warm-variants "$mode" "$dedup"
      done
    done
    done
    done
  '
