#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
docker run --rm --name "galaxy-draw-reject-$$" --pull=never --platform linux/arm64 --network none --cpus=2 \
  --mount "type=bind,source=$root,target=/work,readonly" -w /work \
  -e EGL_PLATFORM=surfaceless -e LIBGL_ALWAYS_SOFTWARE=1 -e GALLIUM_DRIVER=softpipe \
  sha256:e17620c766e0840ba07e63f053e225221f6715cb37f2263a54f3357576a0fc23 bash -euo pipefail -c '
    for on in 1 0; do
      clang++ -std=c++17 -stdlib=libc++ -O1 -g -DVGX_NO_NEON -D__VGX_VITA__ -DPORT_RUNTIME_DIAGNOSTICS=0 -DPORT_LOGGING=0 -DVGX_COLDTRACE=0 -DGXV_DRAW_REJECT_COUNTERS=$on \
        -ffunction-sections -fdata-sections -Wl,--gc-sections \
        -Wl,--wrap=glDrawElements,--wrap=glFinish,--wrap=glGetError,--wrap=glReadPixels,--wrap=getenv,--wrap=glCreateProgram \
        -Wl,--wrap=printf,--wrap=vprintf,--wrap=fprintf,--wrap=vfprintf,--wrap=puts,--wrap=putchar,--wrap=fputs,--wrap=fputc,--wrap=putc,--wrap=fwrite \
        -Itests/autoq_clock_stubs -Iplatform/include -Iplatform/vgx/src -Iplatform/vgx/third_party \
        -fsanitize=address,undefined -fno-sanitize-link-runtime -fno-omit-frame-pointer \
        tests/vgx_draw_reject.cpp platform/vgx/src/vgx_shadergen.cpp vita/vgx-game/silent_stdio.cpp \
        -o /tmp/draw-reject-$on -lEGL -lGLESv2 -pthread -l:libasan.so.8 -l:libubsan.so.1
      for scenario in good allocation compile link missing static stream state sanitize skip empty; do
        LD_PRELOAD=libasan.so.8 ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 timeout 30s /tmp/draw-reject-$on "$scenario"
        echo "PASS macro=$on scenario=$scenario"
      done
    done
  '
