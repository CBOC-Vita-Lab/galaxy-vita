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
    extra=();wrap=()
    if test -f vita/vgx-game/coldtrace_wrap.cpp; then
      extra=(vita/vgx-game/coldtrace_wrap.cpp)
      for symbol in sceIoMkdir sceIoOpen sceIoLseek sceIoRead sceIoPread sceIoWrite sceIoClose shark_compile_shader_extended shark_compile_shader sceGxmShaderPatcherRegisterProgram sceGxmShaderPatcherCreateVertexProgram sceGxmShaderPatcherCreateFragmentProgram sceGxmBeginScene sceGxmEndScene sceGxmFinish sceGxmNotificationWait sceGxmTransferFinish; do
        wrap+=("-Wl,--wrap=$symbol")
      done
    fi
    clang++ -std=c++17 -stdlib=libc++ -O1 -g -fno-short-enums -DVGX_COLDTRACE=1 \
      -idirafter /sdk-include -idirafter /vgl-include -fsanitize=address,undefined \
      -fno-sanitize-link-runtime -fno-omit-frame-pointer -pthread \
      tests/vita_coldtrace_wrap.cpp tests/vita_coldtrace_driver.cpp "${extra[@]}" "${wrap[@]}" \
      -l:libasan.so.8 -l:libubsan.so.1 -o /tmp/coldtrace-wrap
    ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 timeout 60s /tmp/coldtrace-wrap
  '
