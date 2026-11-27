#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
mkdir -p "$root/build-gx-xf-test"
docker run --rm --platform linux/arm64 --network none --cpus=2 -v "$root:/work" -w /work \
  codex-galaxy-native:20261007-arm64 clang++ -std=c++20 -stdlib=libc++ -O2 \
  -Iplatform/src/gx tests/gx_cpu_xf.cpp platform/src/gx/cpu_xf.cpp platform/src/gx/gl_shadergen.cpp \
  -lEGL -lGLESv2 -o build-gx-xf-test/gx_cpu_xf
docker run --rm --platform linux/arm64 --network none --cpus=2 -v "$root:/work" -w /work \
  --env EGL_PLATFORM=surfaceless --env LIBGL_ALWAYS_SOFTWARE=1 \
  codex-galaxy-native:20261007-arm64 build-gx-xf-test/gx_cpu_xf
