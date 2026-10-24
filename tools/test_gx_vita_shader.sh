#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
mkdir -p "$root/build-gx-vita-shader"
docker run --rm --pull=never --platform linux/arm64 --network none --cpus=2 \
  --mount "type=bind,source=$root,target=/work" -w /work \
  codex-galaxy-native:20261007-arm64 bash -euo pipefail -c '
    clang++ -std=c++20 -stdlib=libc++ -O2 -Wall -Wextra -Werror \
      -Iplatform/src/gx tests/gx_vita_shader.cpp platform/src/gx/portable_shader.cpp \
      -o build-gx-vita-shader/contracts
    build-gx-vita-shader/contracts build-gx-vita-shader
  '
