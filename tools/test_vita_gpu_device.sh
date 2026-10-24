#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
sdk=${VITASDK:-/opt/vitasdk}
docker run --rm --pull=never --platform linux/arm64 --network none --cpus=2 \
  -e VITA_GPU_SANITIZERS="${VITA_GPU_SANITIZERS:-0}" \
  --mount "type=bind,source=$root,target=/work,readonly" \
  --mount "type=bind,source=$sdk,target=/sdk,readonly" -w /work \
  codex-galaxy-native:20261007-arm64 bash -euo pipefail -c '
    sources=(tests/vita_gpu_device.cpp vita/runtime/gpu_memory.cpp)
    if [[ -f vita/runtime/gpu_device.cpp ]]; then sources+=(vita/runtime/gpu_device.cpp); fi
    if [[ -f vita/runtime/gpu_texture.cpp ]]; then sources+=(vita/runtime/gpu_texture.cpp); fi
    if [[ -f vita/runtime/gpu_staging.cpp ]]; then sources+=(vita/runtime/gpu_staging.cpp); fi
    flags=()
    libraries=()
    if [[ "$VITA_GPU_SANITIZERS" == 1 ]]; then
      flags=(-fsanitize=address,undefined -fno-sanitize-link-runtime -fno-omit-frame-pointer)
      libraries=(-l:libasan.so.8 -l:libubsan.so.1)
      export ASAN_OPTIONS=detect_leaks=1
      export UBSAN_OPTIONS=halt_on_error=1
    fi
    clang++ -std=c++20 -stdlib=libc++ -O2 -Wall -Wextra -Werror \
      -ffunction-sections -Wl,--gc-sections -Iplatform/include -Ivita/runtime -idirafter /sdk/arm-vita-eabi/include \
      "${flags[@]}" "${sources[@]}" -o /tmp/vita_gpu_device "${libraries[@]}"
    /tmp/vita_gpu_device
  '
