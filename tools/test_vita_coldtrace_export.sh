#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
sdk=${VITASDK:-'/opt/vitasdk'}
deps=${VGL_DEPS:-'/opt/galaxy-vgl'}
docker run --rm --pull=never --platform linux/arm64 --network none --cpus=2 \
  --mount "type=bind,source=$root,target=/work,readonly" \
  --mount "type=bind,source=$sdk/arm-vita-eabi/include,target=/sdk-include,readonly" \
  --mount "type=bind,source=$deps/include,target=/vgl-include,readonly" -w /work \
  codex-galaxy-native:20261007-arm64 bash -euo pipefail -c '
    sources=();wraps=()
    if test -f vita/vgx-game/coldtrace_export.cpp; then
      sources=(platform/src/port/perf.cpp vita/vgx-game/coldtrace_export.cpp vita/vgx-game/coldtrace_wrap.cpp)
      wraps=(-Wl,--wrap=sceIoOpen -Wl,--wrap=sceIoWrite -Wl,--wrap=sceIoClose)
    fi
    for diagnostics in 0 1; do
    clang++ -std=c++20 -stdlib=libc++ -O1 -g -DVGX_COLDTRACE=1 -DPORT_RUNTIME_DIAGNOSTICS=$diagnostics -DPORT_LOGGING=0 \
      -ffunction-sections -fdata-sections -Wl,--gc-sections -Iplatform/include -idirafter /sdk-include -idirafter /vgl-include \
      -fsanitize=address,undefined -fno-sanitize-link-runtime -fno-omit-frame-pointer -pthread \
      tests/vita_coldtrace_export.cpp "${sources[@]}" "${wraps[@]}" -l:libasan.so.8 -l:libubsan.so.1 -o /tmp/coldtrace-export
    export ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1
    for mode in ok short exists write-fail zero-write close-fail close-fail-remove-fail; do timeout 30s /tmp/coldtrace-export "$mode"; done
    done
  '
