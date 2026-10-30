#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
sdk=${VITASDK:?set VITASDK to the SDK folder}
driver=${VGX_FROZEN_VGL_SRC:?set VGX_FROZEN_VGL_SRC to the exact prepared driver source}
deps=${VGL_DEPS:?set VGL_DEPS to the library folder with include/vitashark.h}
out=${AUTHORED_MIPS_OUT:-}
if [[ -z $out ]];then out=$(mktemp -d);trap 'rm -rf "$out"' EXIT;fi
mkdir -p "$out"
node "$root/tools/extract_vita_authored_mips.cjs" "$driver" "$out" "$root"
docker run --rm --pull=never --platform linux/arm64 --network none --cpus=2 \
  --mount "type=bind,source=$root,target=/work,readonly" --mount "type=bind,source=$out,target=/fixture" \
  --mount "type=bind,source=$sdk,target=/sdk,readonly" --mount "type=bind,source=$driver,target=/driver,readonly" \
  --mount "type=bind,source=$deps,target=/deps,readonly" \
  -w /work -e "MIP_TEST_MODES=${MIP_TEST_MODES:-ordinary-loss default-off off on controls pattern}" \
  -e EGL_PLATFORM=surfaceless -e LIBGL_ALWAYS_SOFTWARE=1 -e GALLIUM_DRIVER=softpipe \
  codex-galaxy-native:20261007-arm64 bash -euo pipefail -c '
    common=(-O1 -g -ffunction-sections -fdata-sections -fsanitize=address,undefined -fno-sanitize-link-runtime -fno-omit-frame-pointer)
    native=(-Itests -I/fixture -I/driver -I/deps/include -idirafter /sdk/arm-vita-eabi/include)
    for src in /fixture/authored-driver.c tests/vita_authored_mips_driver.c;do
      clang -std=c11 "${common[@]}" "${native[@]}" -DANISO_SWITCH -Wno-switch -Wno-incompatible-function-pointer-types -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast \
        -c "$src" -o /fixture/$(basename "$src").o
    done
    clang++ -std=c++17 -stdlib=libc++ "${common[@]}" -DVGX_NO_NEON -D__VGX_VITA__ -DPORT_RUNTIME_DIAGNOSTICS=0 -DPORT_LOGGING=0 \
      -Itests/autoq_clock_stubs -Iplatform/include -Iplatform/vgx/src -Iplatform/vgx/third_party -Itests -I/fixture -Wl,--gc-sections \
      -Wl,--wrap=glDrawElements,--wrap=glFinish,--wrap=glGetError,--wrap=glReadPixels,--wrap=getenv \
      -Wl,--wrap=glTexImage2D,--wrap=glTexSubImage2D,--wrap=glGenerateMipmap \
      -Wl,--wrap=printf,--wrap=vprintf,--wrap=fprintf,--wrap=vfprintf,--wrap=puts,--wrap=putchar,--wrap=fputs,--wrap=fputc,--wrap=putc,--wrap=fwrite \
      tests/vita_authored_mips.cpp platform/vgx/src/vgx_shadergen.cpp vita/vgx-game/silent_stdio.cpp \
      platform/vgx/src/vgx_state.cpp platform/vgx/src/vgx_draw.cpp platform/vgx/src/vgx_fesplit.cpp platform/vgx/src/vgx_dlcache.cpp \
      platform/vgx/src/vgx_guestmem.cpp platform/vgx/src/vgx_bpsync.cpp platform/vgx/src/vgx_texcache.cpp platform/vgx/src/vgx_texdecode.cpp platform/vgx/src/vgx_stats.cpp \
      /fixture/*.c.o -o /fixture/authored-mips -lEGL -lGLESv2 -pthread -lm -l:libasan.so.8 -l:libubsan.so.1
    for mode in $MIP_TEST_MODES;do
      flag=1;test "$mode" != off || flag=0
      setting=("GXV_AUTHORED_I4_MIPS=$flag");test "$mode" != default-off || setting=()
      gens=1;syncs=1;if test "$mode" = on;then gens="0 1";syncs="0 1";fi
      for gen in $gens;do for sync in $syncs;do
        printf "CASE mode=%s gen=%s sync=%s\n" "$mode" "$gen" "$sync"
        env -u GXV_AUTHORED_I4_MIPS "${setting[@]}" LD_PRELOAD=libasan.so.8 ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 \
          GXV_MIPGEN=$gen GXV_MIPCLAMP=1 GXV_LA88=1 GXV_VITA_UPLOAD_SYNC=$sync timeout 120s /fixture/authored-mips "$mode"
      done;done
    done
  '
