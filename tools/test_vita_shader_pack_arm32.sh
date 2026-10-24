#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
sdk=${VITASDK:-'/opt/vitasdk'}
vgl=${VGL_DEPS:-'/opt/galaxy-vgl'}
out=$(mktemp -d "${TMPDIR:-/tmp}/vgx-shader-pack-arm32.XXXXXX")
trap 'rm -rf "$out"' EXIT
node "$root/tools/prepare_vita_shader_reader.cjs" "$vgl" "$out/driver"
node "$root/tools/extract_vita_shader_cache_fixture.cjs" "$vgl" "$out" "$out/driver/candidate/custom_shaders.c"
docker run --rm --pull=never --platform linux/arm64 --network none --cpus=2 \
  --mount "type=bind,source=$root,target=/work,readonly" --mount "type=bind,source=$out,target=/results" \
  --mount "type=bind,source=$sdk,target=/sdk,readonly" --mount "type=bind,source=$vgl,target=/vgl,readonly" -w /work \
  codex-strikers-arm-tests:20261006 bash -euo pipefail -c '
    run=(qemu-arm -cpu cortex-a9 -L /usr/arm-linux-gnueabihf)
    base=(-O2 -g -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -fno-pie -Wno-psabi)
    arm-linux-gnueabihf-g++ -std=c++17 "${base[@]}" -no-pie -Iplatform/vgx/third_party \
      -Wl,--wrap=malloc -Wl,--wrap=fread -Wl,--wrap=__fread_chk -Wl,--wrap=fclose tests/vita_shader_pack.cpp -o /results/unit -pthread
    timeout 60s "${run[@]}" /results/unit
    includes=(-Iplatform/include -Iplatform/vgx/vita/include -Iplatform/vgx/vita -I/vgl/include -I/vgl/src -Iplatform/vgx/src
      -Iplatform/vgx/third_party -Itests -I/results -idirafter /sdk/arm-vita-eabi/include)
    flags=("${base[@]}" -D__vita__ -D__VGX_VITA__ -DNOMINMAX -fno-short-enums -ffunction-sections -fdata-sections
      -DPORT_RUNTIME_DIAGNOSTICS=0 -DPORT_LOGGING=0 -DGXV_SHADER_CACHE_PACK=1 -DGXV_SHADER_CACHE_READER=1
      -DHAVE_SHADER_CACHE -DHAVE_SHARK_LOG -DLOG_ERRORS)
    arm-linux-gnueabihf-gcc -std=c11 "${flags[@]}" "${includes[@]}" -DglShaderSource=frozen_glShaderSource \
      -Wno-format -c /results/frozen-shader-cache.c -o /results/frozen.o
    arm-linux-gnueabihf-g++ -std=c++17 "${base[@]}" -no-pie -Iplatform/vgx/third_party tools/vita_shader_pack.cpp -o /results/build-pack
    for trace in 0 1; do
      wraps=(-Wl,--wrap=sceIoOpen -Wl,--wrap=vglMalloc -Wl,--wrap=vglProgramGetParameterBase -Wl,--wrap=malloc)
      if test "$trace" = 1; then
        for name in sceIoMkdir sceIoLseek sceIoRead sceIoWrite sceIoClose; do wraps+=("-Wl,--wrap=$name"); done
      fi
      arm-linux-gnueabihf-g++ -std=c++20 "${flags[@]}" "${includes[@]}" -DVGX_COLDTRACE=$trace \
        -no-pie -Wl,--gc-sections "${wraps[@]}" \
        tests/vita_shader_pack_driver.cpp tests/vita_shader_cache_driver.cpp vita/vgx-game/coldtrace_wrap.cpp \
        /results/frozen.o -o /results/driver-$trace -pthread -latomic
      base=/results/cache-$trace
      identity=$("${run[@]}" /results/driver-$trace prepare "$base")
      folders=("$base"/*); test "${#folders[@]}" = 1; folder=${folders[0]}
      "${run[@]}" /results/build-pack "$folder/v2" "$identity" "$folder/stages.pack"
      GXV_SHADERPACK=0 timeout 60s "${run[@]}" /results/driver-$trace off "$base"
      GXV_SHADERPACK=1 timeout 60s "${run[@]}" /results/driver-$trace oom "$base"
      GXV_SHADERPACK=1 timeout 60s "${run[@]}" /results/driver-$trace run "$base"
    done
    printf "%s\n" "PASS: ARM32 Cortex-A9 functional checks only; QEMU is not native performance evidence"
  '
