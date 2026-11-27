#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
sdk=${VITASDK:-/opt/vitasdk}
mkdir -p "$root/local/render-accounting-tests"
docker run --rm --pull=never --platform linux/arm64 --network none --cpus=2 \
  --mount "type=bind,source=$root,target=/work,readonly" \
  --mount "type=bind,source=$root/local/render-accounting-tests,target=/results" \
  --mount "type=bind,source=$sdk,target=/sdk,readonly" -w /work \
  codex-galaxy-native:20261007-arm64 bash -euo pipefail -c '
    export ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1
    common=(-std=c++20 -stdlib=libc++ -O1 -g -D__vita__ -DTARGET_PC=1 -DNDEBUG=1 -fms-extensions
      -DPORT_GX_VGX=1 -DPORT_GX_VGX_ONLY=1 -ffunction-sections -fdata-sections -Wl,--gc-sections
      -Iplatform/include -Ivita/runtime -Iplatform/vgx/include -Iplatform/vgx/src
      -Idecomp/libs/RVL_SDK/include -idirafter /sdk/arm-vita-eabi/include
      -include platform/include/port/compat.h -Wno-register -Wno-unknown-pragmas
      -Wno-deprecated-volatile -Wno-deprecated-declarations
      -fsanitize=address,undefined -fno-sanitize-link-runtime -fno-omit-frame-pointer)
    for diagnostics in 0 1; do
      clang++ "${common[@]}" -DPORT_RUNTIME_DIAGNOSTICS=$diagnostics tests/vgx_render_accounting.cpp \
        -o /results/render-$diagnostics -l:libasan.so.8 -l:libubsan.so.1
      timeout 20s /results/render-$diagnostics
    done
  '
