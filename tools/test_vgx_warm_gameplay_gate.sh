#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
src="$root/platform/vgx/src"
san=(-fsanitize=address,undefined -fno-sanitize-link-runtime -fno-omit-frame-pointer)
libs=(-l:libasan.so.8 -l:libubsan.so.1)
"${CXX:-clang++}" -std=c++17 -O1 -g -DVGX_NO_NEON "${san[@]}" -I"$src" -c "$src/vgx_autoq.cpp" -o "$out/warm-gate-autoq.o"
for hooks in 7 3 5 6 0; do
  for diagnostics in 0 1; do
    for trace in 0 1; do
      if [[ "$hooks" != 7 && ( "$diagnostics" != 0 || "$trace" != 0 ) ]]; then continue; fi
      tag="$hooks-$diagnostics-$trace"
      "${CXX:-clang++}" -std=c++17 -O1 -g -DVGX_NO_NEON -D__VGX_VITA__ -DWARM_HOOKS="$hooks" \
        -DPORT_RUNTIME_DIAGNOSTICS="$diagnostics" -DVGX_COLDTRACE="$trace" "${san[@]}" \
        -ffunction-sections -fdata-sections -Wl,--gc-sections -I"$src" -I"$root/platform/vgx/third_party" \
        "$root/tests/vgx_warm_gameplay_gate.cpp" "$src/vgx_shadergen.cpp" "$out/warm-gate-autoq.o" \
        -o "$out/warm-gameplay-$tag" -Wl,--wrap=glDrawElements -Wl,--wrap=glCreateProgram "${libs[@]}" -lEGL -lGLESv2 -pthread
      cases=(legacy)
      if [[ "$hooks" == 7 ]]; then cases=(blocked unknown startup-gl startup-yielded demand guards failure); fi
      if [[ "$hooks" == 3 ]]; then cases=(missing-splash); fi
      for mode in "${cases[@]}"; do
        env -i PATH="$PATH" HOME="${HOME:-/tmp}" EGL_PLATFORM=surfaceless LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=softpipe \
          ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 GXV_LOG=0 GXV_AUTOQ=0 GXV_WORKLOAD_PROBE=0 \
          GXV_FXHALF=0 GXV_PASSHALF=0 GXV_WARM_BACKOFF=1 GXV_WARM_DEDUP=1 \
          "$out/warm-gameplay-$tag" "$mode"
      done
    done
  done
done
