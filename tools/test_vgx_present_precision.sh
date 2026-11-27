#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
docker run --rm --name "galaxy-present-precision-$$" --pull=never --platform linux/arm64 --network none --cpus=2 \
  --mount "type=bind,source=$root,target=/work,readonly" -w /work \
  -e EGL_PLATFORM=surfaceless -e LIBGL_ALWAYS_SOFTWARE=1 -e GALLIUM_DRIVER=llvmpipe -e LP_NUM_THREADS=2 \
  codex-galaxy-native:20261007-arm64 bash -euo pipefail -c '
    python3 -I tests/vgx_present_precision_sources.py /tmp/present-sources.inc
    common=(-std=c++17 -stdlib=libc++ -O1 -g -DVGX_NO_NEON -D__VGX_VITA__ -DPORT_LOGGING=0 -DVGX_COLDTRACE=0
      -ffunction-sections -fdata-sections -Wl,--gc-sections
      -Itests/autoq_clock_stubs -Iplatform/include -Iplatform/vgx/src -Iplatform/vgx/third_party -I/tmp
      -fsanitize=address,undefined -fno-sanitize-link-runtime -fno-omit-frame-pointer)
    wraps=(-Wl,--wrap=glDrawElements,--wrap=glFinish,--wrap=glGetError,--wrap=glReadPixels,--wrap=getenv)
    libs=(-lEGL -lGLESv2 -pthread -l:libasan.so.8 -l:libubsan.so.1)
    clang++ "${common[@]}" -ffp-contract=off tests/vgx_present_precision_numeric.cpp -o /tmp/present-numeric -l:libasan.so.8 -l:libubsan.so.1
    clang++ "${common[@]}" "${wraps[@]}" -Wl,--wrap=glCompileShader,--wrap=glDeleteShader,--wrap=glCreateProgram,--wrap=glDeleteProgram,--wrap=glLinkProgram,--wrap=glGetProgramiv \
      tests/vgx_present_precision_fallback.cpp platform/vgx/src/vgx_shadergen.cpp -o /tmp/present-fallback "${libs[@]}"
    clang++ "${common[@]}" "${wraps[@]}" -Wl,--wrap=glDrawArrays \
      tests/vgx_present_precision_pixels.cpp platform/vgx/src/vgx_shadergen.cpp -o /tmp/present-pixels "${libs[@]}"
    export LD_PRELOAD=libasan.so.8 ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1
    timeout -k 5 120 /tmp/present-numeric
    for mode in success half-compile half-link both-compile both-link vertex; do /tmp/present-fallback "$mode"; done
    timeout -k 5 180 /tmp/present-pixels
  '
