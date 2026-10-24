#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
docker run --rm --pull=never --platform linux/arm64 --network none --cpus=2 \
  --mount "type=bind,source=$root,target=/work,readonly" -w /work \
  codex-galaxy-native:20261007-arm64 bash -euo pipefail -c '
    clang++ -std=c++20 -stdlib=libc++ -O2 -Wall -Wextra -Werror \
      -Iplatform/src/gx tests/gx_copy_pixels_benchmark.cpp \
      platform/src/gx/gx_copy_pixels.cpp platform/src/gx/gx_copy_bytes.cpp \
      -o /tmp/copy_benchmark
    timeout 20s /tmp/copy_benchmark
    timeout 20s /tmp/copy_benchmark filtered
  '
