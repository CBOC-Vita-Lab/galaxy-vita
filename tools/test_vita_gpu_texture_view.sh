#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
sdk=${VITASDK:-/opt/vitasdk}
docker run --rm --pull=never --platform linux/arm64 --network none --cpus=2 \
  --mount "type=bind,source=$root,target=/work,readonly" \
  --mount "type=bind,source=$sdk,target=/sdk,readonly" -w /work \
  codex-galaxy-native:20261007-arm64 bash -euo pipefail -c '
    export ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1
    clang++ -std=c++20 -stdlib=libc++ -O1 -g -Wall -Wextra -Werror \
      -ffunction-sections -Wl,--gc-sections -Iplatform/include -Ivita/runtime -idirafter /sdk/arm-vita-eabi/include \
      -fsanitize=address,undefined -fno-sanitize-link-runtime -fno-omit-frame-pointer \
      tests/vita_gpu_texture_view.cpp vita/runtime/gpu_memory.cpp vita/runtime/gpu_device.cpp \
      vita/runtime/gpu_texture.cpp vita/runtime/gpu_staging.cpp \
      -o /tmp/vita_gpu_texture_view -l:libasan.so.8 -l:libubsan.so.1
    /tmp/vita_gpu_texture_view
  '
