#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
docker run --rm --pull=never --platform linux/arm64 --network none --cpus=2 \
  --mount "type=bind,source=$root,target=/work,readonly" -w /work \
  codex-galaxy-native:20261007-arm64 bash -euo pipefail -c '
    export ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1
    clang++ -std=c++17 -stdlib=libc++ -O2 -g -Iplatform/vgx/third_party \
      -fsanitize=address,undefined -fno-sanitize-link-runtime -fno-omit-frame-pointer \
      -Wl,--wrap=malloc -Wl,--wrap=fread -Wl,--wrap=__fread_chk -Wl,--wrap=fclose tests/vita_shader_pack.cpp \
      -o /tmp/shader-pack -l:libasan.so.8 -l:libubsan.so.1 -pthread
    timeout 30s /tmp/shader-pack
  '
