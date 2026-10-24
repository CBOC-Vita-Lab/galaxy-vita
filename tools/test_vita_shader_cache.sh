#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
sdk=${VITASDK:-'/opt/vitasdk'}
vgl=${VGL_DEPS:-'/opt/galaxy-vgl'}
out="$root/local/shader-cache-tests"
if test "${CACHE_TSAN:-0}" = 1; then out="$out-tsan"; fi
mkdir -p "$out"
node "$root/tools/extract_vita_shader_cache_fixture.cjs" "$vgl" "$out"
docker run --rm --pull=never --platform linux/arm64 --network none --cpus=2 \
  --mount "type=bind,source=$root,target=/work,readonly" \
  --mount "type=bind,source=$out,target=/results" \
  --mount "type=bind,source=$sdk,target=/sdk,readonly" \
  --mount "type=bind,source=$vgl,target=/vgl,readonly" -w /work \
  -e "CACHE_CASE=${1:-}" -e "CACHE_TSAN=${CACHE_TSAN:-0}" codex-galaxy-native:20261007-arm64 bash -euo pipefail -c '
    export ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 TSAN_OPTIONS=halt_on_error=1
    sanitize=(-fsanitize=address,undefined); libs=(-l:libasan.so.8 -l:libubsan.so.1)
    if test "$CACHE_TSAN" = 1; then sanitize=(-fsanitize=thread); libs=(-l:libtsan.so.2); fi
    includes=(-Iplatform/include -Iplatform/vgx/vita/include -I/vgl/include -I/vgl/src -Iplatform/vgx/src
      -Iplatform/vgx/third_party -Itests -I/results -idirafter /sdk/arm-vita-eabi/include)
    common=(-O1 -g -D__vita__ -D__VGX_VITA__ -DNOMINMAX -fno-short-enums -ffunction-sections -fdata-sections
      -DPORT_RUNTIME_DIAGNOSTICS=0 -DPORT_LOGGING=0 -DHAVE_SHADER_CACHE -DHAVE_SHARK_LOG -DLOG_ERRORS
      "${sanitize[@]}" -fno-omit-frame-pointer)
    clang -std=c11 "${common[@]}" "${includes[@]}" -DglShaderSource=frozen_glShaderSource \
      -Wno-format -c /results/frozen-shader-cache.c -o /results/frozen-shader-cache.o
    clang -std=c11 "${common[@]}" "${includes[@]}" -DglShaderSource=frozen_glShaderSource -DCACHE_WEAK_ROOT=1 \
      -Wno-format -c /results/frozen-shader-cache.c -o /results/frozen-shader-cache-weak.o
    for trace in 0 1; do
      wraps=(-Wl,--wrap=sceIoOpen)
      if test "$trace" = 1; then
        for name in sceIoMkdir sceIoLseek sceIoRead sceIoWrite sceIoClose; do wraps+=("-Wl,--wrap=$name"); done
      fi
      clang++ -std=c++20 -stdlib=libc++ "${common[@]}" "${includes[@]}" -DVGX_COLDTRACE=$trace \
        -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast -Wno-asm-operand-widths \
        -fno-sanitize-link-runtime -Wl,--gc-sections "${wraps[@]}" \
        tests/vita_shader_cache.cpp tests/vita_shader_cache_driver.cpp vita/vgx-game/coldtrace_wrap.cpp /results/frozen-shader-cache.o \
        -o /results/cache-$trace "${libs[@]}" -pthread
      if test -n "$CACHE_CASE"; then timeout 20s /results/cache-$trace "$CACHE_CASE"; continue; fi
      for mode in hit hit-fragment miss miss-multi miss-nul miss-large warm-buckets write-failure grammar all-buckets root-change concurrent concurrent-buckets max-root trailing-root errno retry missing-parent exists blocked empty unterminated; do
        timeout 20s /results/cache-$trace "$mode"
      done
      clang++ -std=c++20 -stdlib=libc++ "${common[@]}" "${includes[@]}" -DVGX_COLDTRACE=$trace -DCACHE_WEAK_ROOT=1 \
        -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast -Wno-asm-operand-widths \
        -fno-sanitize-link-runtime -Wl,--gc-sections "${wraps[@]}" \
        tests/vita_shader_cache.cpp tests/vita_shader_cache_driver.cpp vita/vgx-game/coldtrace_wrap.cpp /results/frozen-shader-cache-weak.o \
        -o /results/cache-weak-$trace "${libs[@]}" -pthread
      timeout 20s /results/cache-weak-$trace weak-root
    done
  '
