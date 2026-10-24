#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
docker run --rm --pull=never --platform linux/arm64 --network none --cpus=2 \
  --mount "type=bind,source=$root,target=/work,readonly" -w /work -e "COLDTRACE_TSAN=${COLDTRACE_TSAN:-0}" \
  codex-galaxy-native:20261007-arm64 bash -euo pipefail -c '
    sanitize=(-fsanitize=address,undefined -l:libasan.so.8 -l:libubsan.so.1)
    if test "$COLDTRACE_TSAN" = 1; then sanitize=(-fsanitize=thread -l:libtsan.so.2); fi
    clang++ -std=c++20 -stdlib=libc++ -O1 -g -DVGX_COLDTRACE=1 -pthread \
      -fno-sanitize-link-runtime -fno-omit-frame-pointer tests/vgx_coldtrace_control.cpp \
      "${sanitize[@]}" -o /tmp/coldtrace-control
    ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 TSAN_OPTIONS=halt_on_error=1 timeout 30s /tmp/coldtrace-control
  '
