#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
docker run --rm --pull=never --platform linux/arm64 --network none --cpus=2 \
  --mount "type=bind,source=$root,target=/work,readonly" -w /work -e "COLDTRACE_TSAN=${COLDTRACE_TSAN:-0}" -e "COLDTRACE_ONLY=${1:-all}" \
  codex-galaxy-native:20261007-arm64 bash -euo pipefail -c '
    if test "$COLDTRACE_ONLY" != stack; then
    sanitizers=(-fsanitize=address,undefined -l:libasan.so.8 -l:libubsan.so.1)
    if test "$COLDTRACE_TSAN" = 1; then sanitizers=(-fsanitize=thread -l:libtsan.so.2); fi
    clang++ -std=c++17 -stdlib=libc++ -O1 -g -DVGX_COLDTRACE=1 -ffunction-sections -fdata-sections \
      -Wl,--gc-sections -pthread -fno-sanitize-link-runtime -fno-omit-frame-pointer \
      tests/vgx_coldtrace.cpp -o /tmp/coldtrace "${sanitizers[@]}"
    ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 TSAN_OPTIONS=halt_on_error=1 timeout 60s /tmp/coldtrace
    fi
    if test "$COLDTRACE_ONLY" != core; then
    clang++ -std=c++17 -stdlib=libc++ -O2 -DVGX_COLDTRACE=1 -fstack-usage -c tests/vgx_coldtrace_stack.cpp -o /tmp/coldtrace-stack.o
    python3 tests/vgx_coldtrace_stack.py /tmp/coldtrace-stack.su /tmp/coldtrace-stack.o
    clang++ -std=c++17 -stdlib=libc++ -O2 -DVGX_COLDTRACE=0 -c tests/vgx_coldtrace_stack.cpp -o /tmp/coldtrace-off.o
    test -z "$(nm --defined-only /tmp/coldtrace-off.o)"
    echo "PASS: coldtrace0 emits no probe code or storage"
    fi
  '
