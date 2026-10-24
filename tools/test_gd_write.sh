#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
docker run --rm --pull=never --platform linux/arm64 --network none --cpus=2 \
    --mount "type=bind,source=$root,target=/work,readonly" -w /work \
    codex-galaxy-native:20261007-arm64 bash -euo pipefail -c '
    clang++ -std=c++20 -stdlib=libc++ -O1 -g -fms-extensions -DTARGET_PC=1 \
        -fno-strict-aliasing -ffp-contract=on -fno-fast-math -Wno-deprecated-enum-enum-conversion \
        -fsanitize=address,undefined -fno-sanitize-link-runtime -fno-omit-frame-pointer \
        -Iplatform/include -Idecomp/libs/JSystem/include -Idecomp/libs/RVL_SDK/include -Idecomp/include \
        tests/gd_write.cpp tests/gd_write_driver.cpp -o /tmp/gd_write \
        -l:libasan.so.8 -l:libubsan.so.1
    ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 /tmp/gd_write
    '
