#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d "${TMPDIR:-/tmp}/dsp-mix-kernels.XXXXXX")}
mkdir -p "$out"
export ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1
for extra in '' -DPORT_DSP_NO_NEON; do
    "${CXX:-clang++}" -std=c++20 -O2 -g -fsanitize=address,undefined -fno-omit-frame-pointer $extra \
        -I"$root/platform/src/audio" "$root/tests/dsp_mix_kernels.cpp" -o "$out/dsp-kernels"
    for mode in 0 1 2 -1 3; do GXV_DSPNEON=$mode "$out/dsp-kernels"; done
done
"${CXX:-clang++}" -std=c++20 -O2 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
    -fms-extensions -DTARGET_PC=1 -DPORT_PLATFORM_CODE=1 -DPORT_RUNTIME_DIAGNOSTICS=0 \
    -I"$root/platform/include" -I"$root/decomp/include" -I"$root/decomp/libs/JSystem/include" \
    -I"$root/decomp/libs/RVL_SDK/include" -I"$root/decomp/libs/nw4r/include" -I"$root/decomp/libs/RVLFaceLib/include" \
    -include "$root/platform/include/port/compat.h" -Wno-register -Wno-unknown-pragmas \
    "$root/tests/dsp_mixer_kernels.cpp" -o "$out/dsp-mixer"
for mode in 0 1 2; do GXV_DSPNEON=$mode "$out/dsp-mixer"; done
