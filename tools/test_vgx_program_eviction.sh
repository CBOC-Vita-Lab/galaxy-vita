#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
docker run --rm --pull=never --platform linux/arm64 --network none --cpus=2 \
  --mount "type=bind,source=$root,target=/work,readonly" -w /work \
  -e EGL_PLATFORM=surfaceless -e LIBGL_ALWAYS_SOFTWARE=1 -e GALLIUM_DRIVER=softpipe \
  codex-galaxy-native:20261007-arm64 bash -euo pipefail -c '
    clang++ -std=c++17 -stdlib=libc++ -O1 -g -DVGX_NO_NEON -D__VGX_VITA__ -DPORT_RUNTIME_DIAGNOSTICS=0 -DPORT_LOGGING=0 \
      -ffunction-sections -fdata-sections -Wl,--gc-sections \
      -Wl,--wrap=glDrawElements,--wrap=glFinish,--wrap=glGetError,--wrap=glReadPixels,--wrap=getenv,--wrap=glCreateProgram,--wrap=glDeleteProgram \
      -Wl,--wrap=printf,--wrap=vprintf,--wrap=fprintf,--wrap=vfprintf,--wrap=puts,--wrap=putchar,--wrap=fputs,--wrap=fputc,--wrap=putc,--wrap=fwrite \
      -Iplatform/include -Iplatform/vgx/src -Iplatform/vgx/third_party \
      -fsanitize=address,undefined -fno-sanitize-link-runtime -fno-omit-frame-pointer \
      tests/vgx_program_eviction.cpp platform/vgx/src/vgx_shadergen.cpp vita/vgx-game/silent_stdio.cpp \
      -o /tmp/program-eviction -lEGL -lGLESv2 -pthread -l:libasan.so.8 -l:libubsan.so.1
    export LD_PRELOAD=libasan.so.8 ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1
    result=0
    for mode in relink capacity used-pressure burst; do
      if timeout 60s /tmp/program-eviction "$mode"; then :; else result=1; fi
    done
    exit "$result"
  '
