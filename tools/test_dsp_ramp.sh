#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d "${TMPDIR:-/tmp}/dsp-ramp.XXXXXX")}
mkdir -p "$out"
export ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1
flags=(-std=c++20 -fwrapv -fms-extensions -DTARGET_PC=1 -DPORT_PLATFORM_CODE=1 -DPORT_RUNTIME_DIAGNOSTICS=0
    -I"$root/platform/include" -I"$root/decomp/include" -I"$root/decomp/libs/JSystem/include"
    -I"$root/decomp/libs/RVL_SDK/include" -I"$root/decomp/libs/nw4r/include" -I"$root/decomp/libs/RVLFaceLib/include"
    -include string -include "$root/platform/include/port/compat.h" -Wno-register -Wno-unknown-pragmas)
for extra in '' -DPORT_DSPRAMP_NO_NEON; do
    "${CXX:-clang++}" "${flags[@]}" -O2 -g -fsanitize=address,undefined -fno-omit-frame-pointer $extra \
        "$root/tests/dsp_ramp_mixer.cpp" -o "$out/dsp-ramp-mixer"
    for mode in 2 0 1 -1 3 '' garbage 1bad; do GXV_DSPRAMP=$mode "$out/dsp-ramp-mixer"; done
    env -u GXV_DSPRAMP "$out/dsp-ramp-mixer"
done
"${CXX:-clang++}" "${flags[@]}" -O2 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
    "$root/tests/dsp_ramp_oracle.cpp" -o "$out/dsp-ramp-oracle"
GXV_DSPRAMP=2 "$out/dsp-ramp-oracle"
if [[ ${BENCHMARK:-0} == 1 ]]; then
    "${CXX:-clang++}" "${flags[@]}" -O3 "$root/tests/dsp_ramp_benchmark.cpp" -o "$out/dsp-ramp-benchmark"
    for path in 0 1 1 0 1 0; do GXV_DSPRAMP=1 "$out/dsp-ramp-benchmark" "$path"; done
fi
