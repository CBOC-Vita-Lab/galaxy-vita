#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
mkdir -p "$root/build-gx-state-test"
docker run --rm --platform linux/arm64 --network none -v "$root:/work" -w /work \
  codex-galaxy-native:20261007-arm64 clang++ -std=c++20 -stdlib=libc++ \
  -Iplatform/src/gx tests/gx_shader_state.cpp platform/src/gx/gl_shadergen.cpp \
  -o build-gx-state-test/gx_shader_state
docker run --rm --platform linux/arm64 --network none -v "$root:/work" -w /work \
  codex-galaxy-native:20261007-arm64 build-gx-state-test/gx_shader_state
