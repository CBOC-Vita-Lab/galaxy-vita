#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
sdk=${VITASDK:-/opt/vitasdk}
extra=(-e "VITA_EFB_DUMP_SHADER=${VITA_EFB_DUMP_SHADER:-0}")
if [[ ${VITA_EFB_DUMP_SHADER:-0} == 1 ]]; then
  mkdir -p "$root/build-gx-vita-shader"
  git -C "$root" check-ignore -q build-gx-vita-shader/probe
  extra+=(--mount "type=bind,source=$root/build-gx-vita-shader,target=/shader-output")
fi
docker run --rm --pull=never --platform linux/arm64 --network none --cpus=2 \
  "${extra[@]}" \
  --mount "type=bind,source=$root,target=/work,readonly" \
  --mount "type=bind,source=$sdk,target=/sdk,readonly" -w /work \
  codex-galaxy-native:20261007-arm64 bash -euo pipefail -c '
    export ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1
    clang++ -std=c++20 -stdlib=libc++ -O1 -g -DTARGET_PC=1 -DNDEBUG=1 -fms-extensions \
      -ffunction-sections -Wl,--gc-sections -Iplatform/include -Idecomp/libs/RVL_SDK/include \
      -include platform/include/port/compat.h -Wno-register -Wno-unknown-pragmas \
      -fsanitize=address,undefined -fno-sanitize-link-runtime -fno-omit-frame-pointer \
      tests/os_irq.cpp -o /tmp/os_irq -l:libasan.so.8 -l:libubsan.so.1
    scheduling_status=0
    if ! timeout 10s /tmp/os_irq; then scheduling_status=1; fi
    clang++ -std=c++20 -stdlib=libc++ -O1 -g -D__vita__ -DTARGET_PC=1 -DNDEBUG=1 -fms-extensions \
      -ffunction-sections -Iplatform/include -Idecomp/libs/RVL_SDK/include \
      -include platform/include/port/compat.h -Wno-register -Wno-unknown-pragmas \
      -fsanitize=address,undefined -fno-omit-frame-pointer \
      -c platform/src/vi/vi.cpp -o /tmp/vi.o
    clang++ -std=c++20 -stdlib=libc++ -O1 -g -D__vita__ -DTARGET_PC=1 -DNDEBUG=1 -fms-extensions \
      -ffunction-sections -Iplatform/include -Idecomp/libs/RVL_SDK/include \
      -include platform/include/port/compat.h -Wno-register -Wno-unknown-pragmas \
      -fsanitize=address,undefined -fno-omit-frame-pointer \
      -c tests/vi_scanout.cpp -o /tmp/vi_scanout.o
    frame=()
    if test -f platform/src/gx/vita_frame.cpp; then frame=(platform/src/gx/vita_frame.cpp); fi
    if test -f platform/src/gx/vita_texture_cache.cpp; then frame+=(platform/src/gx/vita_texture_cache.cpp); fi
    if test -f platform/src/gx/vita_material_cache.cpp; then frame+=(platform/src/gx/vita_material_cache.cpp); fi
    if test -f platform/src/gx/vita_overlap.cpp; then frame+=(platform/src/gx/vita_overlap.cpp); fi
    if test -f platform/src/gx/vita_shader_bundle.cpp; then frame+=(platform/src/gx/vita_shader_bundle.cpp); fi
    if test -f platform/src/gx/vita_efb.cpp; then frame+=(platform/src/gx/vita_efb.cpp); fi
    if test -f platform/src/gx/vita_xfb.cpp; then frame+=(platform/src/gx/vita_xfb.cpp); fi
    if test -f platform/src/gx/vita_game_worker.cpp; then frame+=(platform/src/gx/vita_game_worker.cpp); fi
    if test -f vita/game/entry.cpp; then frame+=(vita/game/entry.cpp); fi
    if test -f platform/src/gx/gx_copy_bytes.cpp; then frame+=(platform/src/gx/gx_copy_bytes.cpp); fi
    if test -f platform/src/gx/gx_copy_pixels.cpp; then frame+=(platform/src/gx/gx_copy_pixels.cpp); fi
    if test -f platform/src/gx/gx_ram_texture.cpp; then frame+=(platform/src/gx/gx_ram_texture.cpp); fi
    if test -f vita/bringup/renderer.cpp; then frame+=(vita/bringup/renderer.cpp); fi
    if test -f vita/runtime/tex_image.cpp; then frame+=(vita/runtime/tex_image.cpp); fi
    frame+=(platform/src/gx/gpu_cp.cpp platform/src/gx/gx_recorder.cpp)
    clang++ -std=c++20 -stdlib=libc++ -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter -Wno-unused-function \
      -ffunction-sections -Wl,--gc-sections -Wl,--wrap=fdopen -Wl,--wrap=fclose -Iplatform/src/gx -Iplatform/include -Ivita/runtime -idirafter /sdk/arm-vita-eabi/include \
      -fsanitize=address,undefined -fno-sanitize-link-runtime -fno-omit-frame-pointer \
      tests/gx_vita_frame.cpp /tmp/vi.o /tmp/vi_scanout.o "${frame[@]}" platform/src/gx/cpu_xf.cpp \
      platform/src/gx/index_chunks.cpp platform/src/gx/draw_spans.cpp \
      platform/src/gx/portable_shader.cpp platform/src/gx/gl_shadergen.cpp \
      platform/src/gx/tex_decode.cpp \
      vita/runtime/gpu_memory.cpp vita/runtime/gpu_device.cpp vita/runtime/gpu_program.cpp \
      vita/runtime/gpu_staging.cpp vita/runtime/gpu_raster.cpp vita/runtime/gpu_texture.cpp \
      -o /tmp/gx_vita_frame -l:libasan.so.8 -l:libubsan.so.1
    timeout 90s /tmp/gx_vita_frame
    routing_status=0
    for mode in native desktop; do
      define=()
      if test "$mode" = native; then define=(-D__vita__); fi
      clang++ -std=c++20 -stdlib=libc++ -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter -Wno-unused-function \
        "${define[@]}" -Iplatform/src/gx -Iplatform/include -ffunction-sections -Wl,--gc-sections \
        -fsanitize=address,undefined -fno-sanitize-link-runtime -fno-omit-frame-pointer \
        tests/gx_live_recorder.cpp platform/src/gx/gpu_cp.cpp platform/src/gx/gx_recorder.cpp \
        platform/src/gx/gpu_null.cpp platform/src/gx/tex_decode.cpp platform/src/gx/draw_spans.cpp \
        platform/src/gx/gl_shadergen.cpp vita/runtime/tex_image.cpp \
        -o "/tmp/gx_live_$mode" -l:libasan.so.8 -l:libubsan.so.1
      if ! "/tmp/gx_live_$mode"; then routing_status=1; fi
    done
    clang -std=c11 -O1 -g -DTARGET_PC=1 -DNDEBUG=1 -fms-extensions \
      -ffunction-sections -fdata-sections -Iplatform/include -Idecomp/libs/RVL_SDK/include \
      -include platform/desktop/c_compat.h -include platform/include/port/compat.h \
      -fsanitize=address,undefined -fno-omit-frame-pointer \
      -c platform/src/sdk/mtx.c -o /tmp/pointer_mtx.o
    for mode in native desktop; do
      define=()
      if test "$mode" = native; then define=(-D__vita__); fi
      clang++ -std=c++20 -stdlib=libc++ -O1 -g -DTARGET_PC=1 -DNDEBUG=1 -fms-extensions \
        "${define[@]}" -ffunction-sections -fdata-sections -Wl,--gc-sections \
        -Iplatform/include -Idecomp/libs/RVL_SDK/include -Idecomp/libs/nw4r/include \
        -isystem decomp/libs/JSystem/include -isystem decomp/include \
        -include platform/include/port/compat.h -Wno-register -Wno-int-to-pointer-cast \
        -Wno-pointer-to-int-cast -Wno-c++11-narrowing -Wno-unknown-pragmas \
        -fsanitize=address,undefined -fno-sanitize-link-runtime -fno-omit-frame-pointer \
        tests/gx_pointer_depth.cpp decomp/src/Game/Screen/StarPointerController.cpp \
        decomp/src/Game/Util/DirectDraw.cpp /tmp/pointer_mtx.o \
        -o "/tmp/gx_pointer_$mode" -l:libasan.so.8 -l:libubsan.so.1
      if ! "/tmp/gx_pointer_$mode"; then routing_status=1; fi
    done
    exit "$((routing_status | scheduling_status))"
  '
