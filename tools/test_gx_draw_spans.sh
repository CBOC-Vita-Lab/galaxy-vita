#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
image=codex-galaxy-native:20261007-arm64
# Existing image only, no network, read-only source, bounded build resources.
test "$(docker image inspect --format '{{.Architecture}}' "$image")" = arm64
case "$(docker info --format '{{.Architecture}}')" in
  arm64|aarch64) ;;
  *) echo 'Native ARM64 Docker engine required; refusing emulated execution.' >&2; exit 1 ;;
esac
docker run --rm --pull=never --platform linux/arm64 --network none --cpus=2 \
  -v "$root:/work:ro" -w /work "$image" bash -euo pipefail -c '
    test "$(uname -m)" = aarch64
    # The image has GCC sanitizer DSOs, but not Clang compiler-rt archives.
    clang++ -std=c++20 -stdlib=libc++ -Wall -Wextra -Werror -Wno-unused-parameter -O1 -g \
      -fsanitize=address,undefined -fno-sanitize-link-runtime -fno-omit-frame-pointer -Iplatform/src/gx \
      tests/gx_draw_spans.cpp platform/src/gx/draw_spans.cpp platform/src/gx/gl_shadergen.cpp \
      -o /tmp/gx_draw_spans -l:libasan.so.8 -l:libubsan.so.1
    ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 /tmp/gx_draw_spans
  '
