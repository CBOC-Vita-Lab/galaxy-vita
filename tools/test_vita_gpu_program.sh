#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
sdk=${VITASDK:-/opt/vitasdk}
docker run --rm --pull=never --platform linux/arm64 --network none --cpus=2 \
  --mount "type=bind,source=$root,target=/work,readonly" \
  --mount "type=bind,source=$sdk,target=/sdk,readonly" -w /work \
  codex-galaxy-native:20261007-arm64 bash -euo pipefail -c '
    sources=(tests/vita_gpu_program.cpp vita/runtime/gpu_program.cpp)
    if [[ -f vita/runtime/gpu_raster.cpp ]]; then sources+=(vita/runtime/gpu_raster.cpp); fi
    clang++ -std=c++20 -stdlib=libc++ -O2 -Wall -Wextra -Werror \
      -Ivita/runtime -Iplatform/src/gx -idirafter /sdk/arm-vita-eabi/include \
      "${sources[@]}" -o /tmp/vita_gpu_program
    /tmp/vita_gpu_program
  '
