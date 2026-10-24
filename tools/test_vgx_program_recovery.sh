#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
docker run --rm --pull=never --platform linux/arm64 --network none --cpus=2 \
  --mount "type=bind,source=$root,target=/work,readonly" -w /work \
  -e "RECOVERY_MODES=${1:-good program-zero program-max shader-zero shader-max missing-source utility-program utility-max utility-shader utility-link init-program init-shader init-link source-error warm-program warm-shader sentinel frame-retry fragment-failure warm-demand-program warm-demand-shader warm-persistent demand-first}" \
  -e "RECOVERY_TRACES=${2:-0 1}" \
  -e EGL_PLATFORM=surfaceless -e LIBGL_ALWAYS_SOFTWARE=1 -e GALLIUM_DRIVER=softpipe \
  codex-galaxy-native:20261007-arm64 bash -euo pipefail -c '
    result=0
    for trace in $RECOVERY_TRACES;do
    clang++ -std=c++17 -stdlib=libc++ -O1 -g -DVGX_NO_NEON -D__VGX_VITA__ -DPORT_RUNTIME_DIAGNOSTICS=0 -DPORT_LOGGING=0 -DVGX_COLDTRACE=$trace \
      -ffunction-sections -fdata-sections -Wl,--gc-sections \
      -Wl,--wrap=glDrawElements,--wrap=glFinish,--wrap=glGetError,--wrap=glReadPixels,--wrap=getenv \
      -Wl,--wrap=glCreateProgram,--wrap=glCreateShader,--wrap=glAttachShader,--wrap=glShaderSource,--wrap=glCompileShader,--wrap=glLinkProgram,--wrap=glDrawArrays \
      -Wl,--wrap=printf,--wrap=vprintf,--wrap=fprintf,--wrap=vfprintf,--wrap=puts,--wrap=putchar,--wrap=fputs,--wrap=fputc,--wrap=putc,--wrap=fwrite \
      -Iplatform/include -Iplatform/vgx/src -Iplatform/vgx/third_party \
      -fsanitize=address,undefined -fno-sanitize-link-runtime -fno-omit-frame-pointer \
      tests/vgx_program_recovery.cpp platform/vgx/src/vgx_shadergen.cpp vita/vgx-game/silent_stdio.cpp \
      -o /tmp/program-recovery -lEGL -lGLESv2 -pthread -l:libasan.so.8 -l:libubsan.so.1
    export LD_PRELOAD=libasan.so.8 ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1
    for mode in $RECOVERY_MODES; do
      if timeout 30s /tmp/program-recovery "$mode"; then :; else result=1; fi
    done
    done
    exit "$result"
  '
