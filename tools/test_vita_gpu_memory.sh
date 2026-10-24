#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
sdk=${VITASDK:-/opt/vitasdk}
docker run --rm --pull=never --platform linux/arm64 --network none --cpus=2 \
  --mount "type=bind,source=$root,target=/work,readonly" \
  --mount "type=bind,source=$sdk,target=/sdk,readonly" -w /work \
  codex-galaxy-native:20261007-arm64 bash -euo pipefail -c '
    clang++ -std=c++20 -stdlib=libc++ -O2 -Wall -Wextra -Werror \
      -Ivita/runtime -idirafter /sdk/arm-vita-eabi/include tests/vita_gpu_memory.cpp \
      vita/runtime/gpu_memory.cpp -o /tmp/vita_gpu_memory
    /tmp/vita_gpu_memory
  '
