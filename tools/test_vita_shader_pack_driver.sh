#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
sdk=${VITASDK:-'/opt/vitasdk'}
vgl=${VGL_DEPS:-'/opt/galaxy-vgl'}
out=$(mktemp -d "${TMPDIR:-/tmp}/vgx-shader-pack-driver.XXXXXX")
trap 'rm -rf "$out"' EXIT
node "$root/tools/prepare_vita_shader_reader.cjs" "$vgl" "$out/driver"
node "$root/tools/extract_vita_shader_cache_fixture.cjs" "$vgl" "$out" "$out/driver/${PACK_DRIVER_VARIANT:-candidate}/custom_shaders.c"
docker run --rm --pull=never --platform linux/arm64 --network none --cpus=2 \
  --mount "type=bind,source=$root,target=/work,readonly" --mount "type=bind,source=$out,target=/results" \
  --mount "type=bind,source=$sdk,target=/sdk,readonly" --mount "type=bind,source=$vgl,target=/vgl,readonly" -w /work \
  codex-galaxy-native:20261007-arm64 bash -euo pipefail -c '
    export ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1
    includes=(-Iplatform/include -Iplatform/vgx/vita/include -Iplatform/vgx/vita -I/vgl/include -I/vgl/src -Iplatform/vgx/src
      -Iplatform/vgx/third_party -Itests -I/results -idirafter /sdk/arm-vita-eabi/include)
    flags=(-O1 -g -D__vita__ -D__VGX_VITA__ -DNOMINMAX -fno-short-enums -ffunction-sections -fdata-sections
      -DPORT_RUNTIME_DIAGNOSTICS=0 -DPORT_LOGGING=0 -DGXV_SHADER_CACHE_PACK=1 -DGXV_SHADER_CACHE_READER=1
      -DHAVE_SHADER_CACHE -DHAVE_SHARK_LOG -DLOG_ERRORS -fsanitize=address,undefined -fno-omit-frame-pointer)
    clang -std=c11 "${flags[@]}" "${includes[@]}" -DglShaderSource=frozen_glShaderSource \
      -Wno-format -c /results/frozen-shader-cache.c -o /results/frozen.o
    clang++ -std=c++17 -stdlib=libc++ -O2 -Iplatform/vgx/third_party tools/vita_shader_pack.cpp -o /results/build-pack
    for trace in 0 1; do
      wraps=(-Wl,--wrap=sceIoOpen -Wl,--wrap=vglMalloc -Wl,--wrap=vglProgramGetParameterBase -Wl,--wrap=malloc)
      if test "$trace" = 1; then
        for name in sceIoMkdir sceIoLseek sceIoRead sceIoWrite sceIoClose; do wraps+=("-Wl,--wrap=$name"); done
      fi
      clang++ -std=c++20 -stdlib=libc++ "${flags[@]}" "${includes[@]}" -DVGX_COLDTRACE=$trace \
        -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast -Wno-asm-operand-widths \
        -fno-sanitize-link-runtime -Wl,--gc-sections "${wraps[@]}" \
        tests/vita_shader_pack_driver.cpp tests/vita_shader_cache_driver.cpp vita/vgx-game/coldtrace_wrap.cpp \
        /results/frozen.o -o /results/pack-$trace -l:libasan.so.8 -l:libubsan.so.1 -pthread
      base=/results/cache-$trace
      identity=$(/results/pack-$trace prepare "$base")
      folders=("$base"/*); test "${#folders[@]}" = 1; folder=${folders[0]}
      /results/build-pack "$folder/v2" "$identity" "$folder/stages.pack"
      GXV_SHADERPACK=0 timeout 30s /results/pack-$trace off "$base"
      GXV_SHADERPACK=1 timeout 30s /results/pack-$trace oom "$base"
      GXV_SHADERPACK=1 timeout 30s /results/pack-$trace run "$base"
    done
  '
