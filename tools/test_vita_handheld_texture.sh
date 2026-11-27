#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
docker run --rm --pull=never --platform linux/arm64 --network none --cpus=2 \
  --mount "type=bind,source=$root,target=/work,readonly" -w /work \
  codex-galaxy-native:20261007-arm64 bash -euo pipefail -c '
    clang++ -std=c++17 -stdlib=libc++ -O1 -g -DPORT_LOGGING=0 \
      -ffunction-sections -fdata-sections -Wl,--gc-sections -Iplatform/include \
      -fsanitize=address,undefined -fno-sanitize-link-runtime -fno-omit-frame-pointer \
      tests/vita_handheld_texture.cpp platform/src/input/control_bindings.cpp -o /tmp/handheld-texture -l:libasan.so.8 -l:libubsan.so.1
    for mode in on off; do
      ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 timeout 20s /tmp/handheld-texture "$mode"
    done
  '
