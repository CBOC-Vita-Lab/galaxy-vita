#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d "${TMPDIR:-/tmp}/dsp-fir.XXXXXX")}
mkdir -p "$out"
export ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1
for extra in '' -DPORT_DSPFIR_NO_NEON; do
    "${CXX:-clang++}" -std=c++20 -O2 -g -fsanitize=address,undefined -fno-omit-frame-pointer $extra \
        -fms-extensions -DTARGET_PC=1 -DPORT_PLATFORM_CODE=1 -DPORT_RUNTIME_DIAGNOSTICS=0 \
        -I"$root/platform/include" -I"$root/decomp/include" -I"$root/decomp/libs/JSystem/include" \
        -I"$root/decomp/libs/RVL_SDK/include" -I"$root/decomp/libs/nw4r/include" -I"$root/decomp/libs/RVLFaceLib/include" \
        -include "$root/platform/include/port/compat.h" -Wno-register -Wno-unknown-pragmas \
        "$root/tests/dsp_fir_mixer.cpp" -o "$out/dsp-fir-mixer"
    for mode in 0 1 2 -1 3 '' garbage 1bad; do GXV_DSPFIR=$mode "$out/dsp-fir-mixer"; done
    env -u GXV_DSPFIR "$out/dsp-fir-mixer"
done
if [[ ${BENCHMARK:-0} == 1 ]]; then
    "${CXX:-clang++}" -std=c++20 -O3 -fms-extensions -DTARGET_PC=1 -DPORT_PLATFORM_CODE=1 -DPORT_RUNTIME_DIAGNOSTICS=0 \
        -I"$root/platform/include" -I"$root/decomp/include" -I"$root/decomp/libs/JSystem/include" \
        -I"$root/decomp/libs/RVL_SDK/include" -I"$root/decomp/libs/nw4r/include" -I"$root/decomp/libs/RVLFaceLib/include" \
        -include "$root/platform/include/port/compat.h" -Wno-register -Wno-unknown-pragmas \
        "$root/tests/dsp_fir_benchmark.cpp" -o "$out/dsp-fir-benchmark"
    for fast in 0 1; do GXV_DSPFIR=1 "$out/dsp-fir-benchmark" "$fast"; done
fi
