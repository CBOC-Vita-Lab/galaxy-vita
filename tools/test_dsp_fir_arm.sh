#!/usr/bin/env bash
# ARM32 Linux cross-check of the actual mixer function, not a Vita executable build or speed prediction.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d "${TMPDIR:-/tmp}/dsp-fir-arm.XXXXXX")}
mkdir -p "$out"
flags=(--target=arm-linux-gnueabihf --gcc-toolchain=/usr -std=c++20 -O2 -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard
    -fms-extensions -DTARGET_PC=1 -DPORT_PLATFORM_CODE=1 -DPORT_RUNTIME_DIAGNOSTICS=0
    -I"$root/platform/include" -I"$root/decomp/include" -I"$root/decomp/libs/JSystem/include"
    -I"$root/decomp/libs/RVL_SDK/include" -I"$root/decomp/libs/nw4r/include" -I"$root/decomp/libs/RVLFaceLib/include"
    -include string -include "$root/platform/include/port/compat.h" -Wno-register -Wno-unknown-pragmas)
"${CXX:-clang++}" "${flags[@]}" "$root/tests/dsp_fir_mixer.cpp" -o "$out/dsp-fir-arm"
for mode in 0 1 2 -1 3; do GXV_DSPFIR=$mode "${QEMU_ARM:-qemu-arm}" -L "${ARM_SYSROOT:-/usr/arm-linux-gnueabihf}" "$out/dsp-fir-arm"; done
"${CXX:-clang++}" "${flags[@]}" -S "$root/tests/dsp_fir_mixer.cpp" -o "$out/dsp-fir-arm.s"
if ! grep -Eq 'v(mull|mlal)\.s16|vaddw\.s32' "$out/dsp-fir-arm.s"; then
    printf 'FAIL: ARM32 assembly did not contain expected widening NEON operations\n' >&2
    exit 1
fi
printf 'PASS: ARM32 Cortex-A9 assembly contains widening NEON operations\n'
