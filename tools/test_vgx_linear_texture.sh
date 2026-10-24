#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
driver=${VGX_FROZEN_VGL:-/opt/galaxy-vgl}
sdk=${VITASDK:-/opt/vitasdk}
out=$(mktemp -d "${TMPDIR:-/tmp}/vgx-linear-texture.XXXXXX")
trap 'rm -rf "$out"' EXIT
node "$root/tools/extract_vgx_linear_fixture.cjs" "$driver" "$out"
docker run --rm --pull=never --platform linux/arm64 --network none --cpus=2 \
  --mount "type=bind,source=$root,target=/work,readonly" \
  --mount "type=bind,source=$out,target=/fixture" \
  --mount "type=bind,source=$sdk,target=/sdk,readonly" -w /work \
  codex-galaxy-native:20261007-arm64 bash -euo pipefail -c '
    flags=(-O1 -g -Itests -idirafter /sdk/arm-vita-eabi/include -fsanitize=address,undefined -fno-sanitize-link-runtime -fno-omit-frame-pointer)
    clang -std=c11 "${flags[@]}" -c /fixture/frozen-linear.c -o /fixture/frozen-linear.o
    clang++ -std=c++17 -stdlib=libc++ "${flags[@]}" tests/vgx_linear_texture.cpp /fixture/frozen-linear.o \
      -o /fixture/linear-texture -l:libasan.so.8 -l:libubsan.so.1
    ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 timeout 20s /fixture/linear-texture
  '
