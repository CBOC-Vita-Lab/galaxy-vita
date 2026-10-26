#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
docker run --rm --pull=never --platform linux/arm64 --network none --cpus=2 \
  --mount "type=bind,source=$root,target=/work,readonly" -w /work \
  codex-galaxy-native:20261007-arm64 bash -euo pipefail -c '
    for native in 0 1; do
      flags=(); if test "$native" = 1; then flags=(-D__vita__); fi
      clang++ -std=c++20 -stdlib=libc++ -O1 -g -pthread \
        -fsanitize=address,undefined -fno-sanitize-link-runtime -fno-omit-frame-pointer \
        -DTARGET_PC=1 -DNDEBUG=1 -fms-extensions -DPORT_ARMHF_HARNESS=1 \
        -DPORT_LOGGING=0 -DPORT_PLATFORM_CODE=1 -DPORT_RUNTIME_DIAGNOSTICS=0 \
        -ffunction-sections -fdata-sections -Wl,--gc-sections \
        -Iplatform/include -Ivita/runtime -Itests/settings_stubs -Idecomp/libs/RVL_SDK/include \
        -include tests/vita_host_clock_prelude.h -Wno-register -Wno-unknown-pragmas \
        -Wno-deprecated-declarations "${flags[@]}" tests/vita_host_clock.cpp \
        -o /tmp/settings-clock -l:libasan.so.8 -l:libubsan.so.1
      for mode in real virtual; do /tmp/settings-clock "$mode"; done
    done
  '
