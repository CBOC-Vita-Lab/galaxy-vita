#!/usr/bin/env bash
# tests/vgx_guestmem_generation.cpp with platform/vgx/src/vgx_guestmem.cpp (ASan/UBSan), plus the concurrent writer test
# (ThreadSanitizer).
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
image=${GALAXY_TEST_IMAGE:-codex-galaxy-native:20261007-arm64}
docker run --rm --pull=never --platform linux/arm64 --network none --cpus=2 \
    --mount "type=bind,source=$root,target=/work,readonly" -w /work \
    --entrypoint bash "$image" -euo pipefail -c '
    clang++ -std=c++20 -O1 -g -Iplatform/vgx/src -fsanitize=address,undefined -fno-omit-frame-pointer \
        tests/vgx_guestmem_generation.cpp platform/vgx/src/vgx_guestmem.cpp -o /tmp/generation
    ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 /tmp/generation
    clang++ -std=c++20 -O1 -g -pthread -Iplatform/vgx/src -fsanitize=thread \
        tests/vgx_guestmem_concurrent.cpp platform/vgx/src/vgx_guestmem.cpp -o /tmp/concurrent
    TSAN_OPTIONS=halt_on_error=1 timeout 90s /tmp/concurrent
    '
