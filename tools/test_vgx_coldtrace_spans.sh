#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
docker run --rm --pull=never --platform linux/arm64 --network none --cpus=2 \
  --mount "type=bind,source=$root,target=/work,readonly" -w /work \
  codex-galaxy-native:20261007-arm64 bash -euo pipefail -c '
    clang++ -std=c++17 -stdlib=libc++ -O1 -g -DVGX_COLDTRACE=1 \
      -fsanitize=address,undefined -fno-sanitize-link-runtime -fno-omit-frame-pointer \
      tests/vgx_coldtrace_spans.cpp -pthread -o /tmp/coldtrace-spans -l:libasan.so.8 -l:libubsan.so.1
    ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 timeout 60s /tmp/coldtrace-spans
  '
