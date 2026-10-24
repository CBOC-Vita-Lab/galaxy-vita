#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
sdk=${VITASDK:-/opt/vitasdk}
mkdir -p "$root/local/quiet-tests"
docker run --rm --pull=never --platform linux/arm64 --network none --cpus=2 \
  --mount "type=bind,source=$root,target=/work,readonly" \
  --mount "type=bind,source=$root/local/quiet-tests,target=/results" \
  --mount "type=bind,source=$sdk,target=/sdk,readonly" -w /work \
  codex-galaxy-native:20261007-arm64 bash -euo pipefail -c '
    export ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1
    common=(-std=c++20 -stdlib=libc++ -O1 -g -D__vita__ -DTARGET_PC=1 -DNDEBUG=1 -fms-extensions
      -DPORT_RUNTIME_DIAGNOSTICS=0 -ffunction-sections -fdata-sections -Wl,--gc-sections
      -Iplatform/include -Ivita/runtime -Idecomp/libs/RVL_SDK/include -idirafter /sdk/arm-vita-eabi/include
      -include platform/include/port/compat.h -Wno-register -Wno-unknown-pragmas
      -Wno-deprecated-volatile -Wno-deprecated-declarations
      -fsanitize=address,undefined -fno-sanitize-link-runtime -fno-omit-frame-pointer)
    clang++ "${common[@]}" tests/vita_perf_audit.cpp -o /results/perf -l:libasan.so.8 -l:libubsan.so.1
    clang++ -std=c++20 -stdlib=libc++ -O1 -g -fsanitize=address,undefined -fno-sanitize-link-runtime \
      -fno-omit-frame-pointer -c platform/src/port/msl_printf.cpp -o /results/msl_printf.o
    clang++ "${common[@]}" tests/vita_quiet_log.cpp /results/msl_printf.o -o /results/logger -l:libasan.so.8 -l:libubsan.so.1
    wraps=()
    for symbol in printf vprintf fprintf vfprintf puts putchar fputs fputc putc fwrite; do
      wraps+=(-Wl,--wrap=$symbol)
    done
    clang++ "${common[@]}" -DPORT_LOGGING=0 -fno-builtin tests/vita_silent_log.cpp vita/vgx-game/silent_stdio.cpp \
      /results/msl_printf.o "${wraps[@]}" -o /results/silent-logger -l:libasan.so.8 -l:libubsan.so.1
    clang++ "${common[@]}" -DPORT_GX_VGX=1 -DPORT_GX_VGX_ONLY=1 -Iplatform/vgx/include -Iplatform/vgx/src \
      tests/vita_quiet_fps.cpp -o /results/fps -l:libasan.so.8 -l:libubsan.so.1
    clang++ "${common[@]}" -DPORT_LOGGING=0 -DPORT_GX_VGX=1 -DPORT_GX_VGX_ONLY=1 -Iplatform/vgx/include -Iplatform/vgx/src \
      tests/vita_quiet_fps.cpp -o /results/silent-fps -l:libasan.so.8 -l:libubsan.so.1
    if test "${1:-}" = logger; then exec /results/logger /results/quiet.log; fi
    if test "${1:-}" = silent; then exec /results/silent-logger /results/silent.log; fi
    if test "${1:-}" = silent-fps; then exec /results/silent-fps; fi
    timeout 10s /results/perf quiet
    timeout 10s /results/logger /results/quiet.log 2>/results/stderr.log
    test ! -s /results/stderr.log
    timeout 10s /results/silent-logger /results/silent.log 2>/results/silent-stderr.log
    test ! -s /results/silent-stderr.log
    echo "PASS: silent logging/stdio do not format or write; ordinary file writes preserved"
    set +e
    /results/silent-logger fatal 2>/results/fatal-stderr.log
    status=$?
    set -e
    test "$status" = 134
    test ! -s /results/fatal-stderr.log
    timeout 10s /results/fps
    timeout 10s /results/silent-fps
    for limit in 0 1 3 8; do
      PETARI_FRAMESKIP=$limit timeout 10s /results/perf cost-model 50000000 12000000
    done
    timeout 10s /results/perf backlog
  ' -- "${1:-}"
