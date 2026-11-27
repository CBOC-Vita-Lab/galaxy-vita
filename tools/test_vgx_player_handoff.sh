#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
sdk=${VITASDK:-/opt/vitasdk}
mkdir -p "$root/local/player-handoff-tests"
docker run --rm --pull=never --platform linux/arm64 --network none --cpus=2 \
  --mount "type=bind,source=$root,target=/work,readonly" \
  --mount "type=bind,source=$root/local/player-handoff-tests,target=/results" \
  --mount "type=bind,source=$sdk,target=/sdk,readonly" -w /work \
  codex-galaxy-native:20261007-arm64 bash -euo pipefail -c '
    common=(-std=c++20 -stdlib=libc++ -O1 -g -D__vita__ -DTARGET_PC=1 -DNDEBUG=1 -fms-extensions
      -DPORT_GX_VGX=1 -DPORT_GX_VGX_ONLY=1 -DPORT_RUNTIME_DIAGNOSTICS=0 -DPORT_LOGGING=0
      -ffunction-sections -fdata-sections -Iplatform/include -Ivita/runtime
      -Iplatform/vgx/include -Iplatform/vgx/src -Idecomp/libs/RVL_SDK/include
      -idirafter /sdk/arm-vita-eabi/include -include platform/include/port/compat.h
      -Wno-register -Wno-unknown-pragmas -Wno-deprecated-volatile -Wno-deprecated-declarations)
    clang++ "${common[@]}" -O2 -S -emit-llvm tests/vgx_player_handoff.cpp -o /results/wake.ll
    if test "${1:-}" != stress; then python3 tests/vgx_wake_ordering.py /results/wake.ll; fi
    clang++ "${common[@]}" -Wl,--gc-sections -fsanitize=address,undefined \
      -fno-sanitize-link-runtime -fno-omit-frame-pointer tests/vgx_player_handoff.cpp \
      -o /results/handoff -l:libasan.so.8 -l:libubsan.so.1
    ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 timeout 30s /results/handoff
  ' -- "${1:-}"
