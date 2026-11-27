#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
docker run --rm --pull=never --platform linux/arm64 --network none --cpus=2 \
  --mount "type=bind,source=$root,target=/work,readonly" -w /work \
  codex-galaxy-native:20261007-arm64 bash -euo pipefail -c '
    flags=(-std=c++17 -stdlib=libc++ -O1 -g -DVGX_NO_NEON -D__VGX_VITA__
      -ffunction-sections -fdata-sections -Iplatform/vgx/src -pthread)
    clang++ "${flags[@]}" -S -emit-llvm tests/vgx_warm_budget.cpp -o /tmp/budget.ll
    python3 tests/vgx_warm_budget_ordering.py /tmp/budget.ll
    clang++ "${flags[@]}" -Wl,--gc-sections -fsanitize=address,undefined \
      -fno-sanitize-link-runtime -fno-omit-frame-pointer tests/vgx_warm_budget.cpp \
      -o /tmp/budget -l:libasan.so.8 -l:libubsan.so.1
    ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 timeout 20s /tmp/budget
  '
