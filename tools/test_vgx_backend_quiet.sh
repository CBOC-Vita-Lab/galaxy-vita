#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
docker run --rm --pull=never --platform linux/arm64 --network none --cpus=2 \
  --mount "type=bind,source=$root,target=/work,readonly" -w /work \
  -e "QUIET_DIAGNOSTICS=${1:-default 1 0}" -e "QUIET_TRACES=${2:-0 1}" -e "QUIET_TARGET=${3:-all}" \
  -e EGL_PLATFORM=surfaceless -e LIBGL_ALWAYS_SOFTWARE=1 -e GALLIUM_DRIVER=softpipe \
  codex-galaxy-native:20261007-arm64 bash -euo pipefail -c '
    builds=0
    for diagnostics in $QUIET_DIAGNOSTICS; do
      options=()
      if test "$diagnostics" != default; then options+=("-DPORT_RUNTIME_DIAGNOSTICS=$diagnostics"); fi
      if test "$QUIET_TARGET" != renderer; then
        clang++ -std=c++17 -stdlib=libc++ -O2 -DVGX_NO_NEON -D__VGX_VITA__ \
          -ffunction-sections -fdata-sections -Wl,--gc-sections -Wl,--wrap=_Znwm \
          -Iplatform/vgx/src "${options[@]}" tests/vgx_variant_census.cpp -o /tmp/variant-census
        /tmp/variant-census
      fi
      if test "$QUIET_TARGET" = census; then continue; fi
      for trace in $QUIET_TRACES; do
        clang++ -std=c++17 -stdlib=libc++ -O1 -g -DVGX_NO_NEON -D__VGX_VITA__ -DPORT_LOGGING=0 -DVGX_COLDTRACE=$trace \
          -ffunction-sections -fdata-sections -Wl,--gc-sections -Wl,--wrap=glDrawElements,--wrap=glFinish,--wrap=glGetError,--wrap=glReadPixels,--wrap=getenv \
          -Wl,--wrap=printf,--wrap=vprintf,--wrap=fprintf,--wrap=vfprintf,--wrap=puts,--wrap=putchar,--wrap=fputs,--wrap=fputc,--wrap=putc,--wrap=fwrite \
          -Iplatform/include -Iplatform/vgx/src -Iplatform/vgx/third_party "${options[@]}" \
          -fsanitize=address,undefined -fno-sanitize-link-runtime -fno-omit-frame-pointer \
          tests/vgx_backend_quiet.cpp platform/vgx/src/vgx_shadergen.cpp vita/vgx-game/silent_stdio.cpp \
          -o /tmp/backend-quiet -lEGL -lGLESv2 -pthread -l:libasan.so.8 -l:libubsan.so.1
        QUIET_TRACE=$trace LD_PRELOAD=libasan.so.8 ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 \
          python3 tests/vgx_backend_quiet.py /tmp/backend-quiet "$diagnostics"
        builds=$((builds+1))
      done
    done
    if test "$QUIET_TARGET" != census && test "$builds" -lt 2; then
      printf "%s\n" "NOTE: one configuration requested; cross-build equivalence was not tested"
    fi
  '
