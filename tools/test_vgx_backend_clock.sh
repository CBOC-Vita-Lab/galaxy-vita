#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
sdk=${VITASDK:-/opt/vitasdk}
docker run --rm --pull=never --platform linux/arm64 --network none --cpus=2 \
  --mount "type=bind,source=$root,target=/work,readonly" \
  --mount "type=bind,source=$sdk,target=/sdk,readonly" -w /work \
  -e "CLOCK_CASES=${1:-clock wrap budget-back budget-forward oversized zero boundary near-boundary budget-wrap large-epoch}" \
  codex-galaxy-native:20261007-arm64 bash -euo pipefail -c '
    export LD_PRELOAD=libasan.so.8 ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1
    for native in 1 0; do
      for diagnostics in 0 1; do
        flags=(-std=c++17 -stdlib=libc++ -O1 -g -DVGX_NO_NEON -D__VGX_VITA__ -DVGX_COLDTRACE=0
          -DPORT_RUNTIME_DIAGNOSTICS=$diagnostics -ffunction-sections -fdata-sections -Wl,--gc-sections
          -Iplatform/vgx/src -Iplatform/vgx/third_party -Iplatform/include -idirafter /sdk/arm-vita-eabi/include)
        if test "$native" = 1; then flags+=(-D__vita__); fi
        if test "$native" = 1; then
          clang++ "${flags[@]}" -UVGX_COLDTRACE -DVGX_COLDTRACE=1 -Wno-unused-command-line-argument \
            -fsyntax-only tests/vgx_backend_clock.cpp
        fi
        clang++ "${flags[@]}" -fsanitize=address,undefined -fno-sanitize-link-runtime -fno-omit-frame-pointer \
          tests/vgx_backend_clock.cpp platform/vgx/src/vgx_shadergen.cpp -o /tmp/backend-clock \
          -lGLESv2 -pthread -l:libasan.so.8 -l:libubsan.so.1
        if test "$native" = 0; then timeout 20s /tmp/backend-clock clock; continue; fi
        for mode in $CLOCK_CASES; do timeout 20s /tmp/backend-clock "$mode"; done
      done
    done
  '
