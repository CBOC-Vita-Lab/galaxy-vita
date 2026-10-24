#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
driver=${VGX_FROZEN_VGL:-/opt/galaxy-vgl}
out=$(mktemp -d "${TMPDIR:-/tmp}/vgx-texture-upload.XXXXXX")
trap 'rm -rf "$out"' EXIT
node "$root/tools/extract_vgx_texture_fixture.cjs" "$driver" "$out" "$root"
docker run --rm --pull=never --platform linux/arm64 --network none --cpus=2 \
  --mount "type=bind,source=$root,target=/work,readonly" \
  --mount "type=bind,source=$out,target=/fixture" -w /work \
  -e "TEXTURE_MODES=${1:-inplace move fallback hazard controls compact}" -e "TEXTURE_TRACES=${2:-0 1}" \
  -e "TEXTURE_NATIVE=${TEXTURE_NATIVE:-1}" \
  -e EGL_PLATFORM=surfaceless -e LIBGL_ALWAYS_SOFTWARE=1 -e GALLIUM_DRIVER=softpipe \
  codex-galaxy-native:20261007-arm64 bash -euo pipefail -c '
    clang -std=c11 -O1 -g -Itests -fsanitize=address,undefined -fno-sanitize-link-runtime \
      -fno-omit-frame-pointer -c /fixture/frozen-texture.c -o /fixture/frozen-texture.o
    native_define=-D__VGX_VITA__
    if [[ $TEXTURE_NATIVE == 0 ]];then native_define=;fi
    for trace in $TEXTURE_TRACES;do
      clang++ -std=c++17 -stdlib=libc++ -O1 -g -DVGX_NO_NEON $native_define -DPORT_RUNTIME_DIAGNOSTICS=0 -DPORT_LOGGING=0 -DVGX_COLDTRACE=$trace \
        -ffunction-sections -fdata-sections -Wl,--gc-sections \
        -Wl,--wrap=glDrawElements,--wrap=glFinish,--wrap=glGetError,--wrap=glReadPixels,--wrap=getenv \
        -Wl,--wrap=glCompressedTexImage2D,--wrap=glTexImage2D,--wrap=glGenerateMipmap \
        -Wl,--wrap=printf,--wrap=vprintf,--wrap=fprintf,--wrap=vfprintf,--wrap=puts,--wrap=putchar,--wrap=fputs,--wrap=fputc,--wrap=putc,--wrap=fwrite \
        -Iplatform/include -Iplatform/vgx/src -Iplatform/vgx/third_party -Itests -I/fixture \
        -fsanitize=address,undefined -fno-sanitize-link-runtime -fno-omit-frame-pointer \
        tests/vgx_texture_upload.cpp platform/vgx/src/vgx_shadergen.cpp vita/vgx-game/silent_stdio.cpp /fixture/frozen-texture.o \
        -o /fixture/texture-upload -lEGL -lGLESv2 -pthread -lm -l:libasan.so.8 -l:libubsan.so.1
      for mode in $TEXTURE_MODES;do
        LD_PRELOAD=libasan.so.8 ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 timeout 120s /fixture/texture-upload "$mode"
      done
    done
  '
