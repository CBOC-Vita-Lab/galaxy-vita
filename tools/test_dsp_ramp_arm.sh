#!/usr/bin/env bash
# Cortex-A9 Linux/QEMU unit test only; does not build a Vita executable.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d "${TMPDIR:-/tmp}/dsp-ramp-arm.XXXXXX")}
mkdir -p "$out"
flags=(--target=arm-linux-gnueabihf --gcc-toolchain=/usr -std=c++20 -O2 -fwrapv -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard
    -fms-extensions -DTARGET_PC=1 -DPORT_PLATFORM_CODE=1 -DPORT_RUNTIME_DIAGNOSTICS=0
    -I"$root/platform/include" -I"$root/decomp/include" -I"$root/decomp/libs/JSystem/include"
    -I"$root/decomp/libs/RVL_SDK/include" -I"$root/decomp/libs/nw4r/include" -I"$root/decomp/libs/RVLFaceLib/include"
    -include string -include "$root/platform/include/port/compat.h" -Wno-register -Wno-unknown-pragmas)
"${CXX:-clang++}" "${flags[@]}" "$root/tests/dsp_ramp_mixer.cpp" -o "$out/dsp-ramp-arm"
for mode in 0 1 2 -1 3; do GXV_DSPRAMP=$mode "${QEMU_ARM:-qemu-arm}" -L "${ARM_SYSROOT:-/usr/arm-linux-gnueabihf}" "$out/dsp-ramp-arm"; done
"${CXX:-clang++}" "${flags[@]}" "$root/tests/dsp_ramp_oracle.cpp" -o "$out/dsp-ramp-oracle"
GXV_DSPRAMP=2 "${QEMU_ARM:-qemu-arm}" -L "${ARM_SYSROOT:-/usr/arm-linux-gnueabihf}" "$out/dsp-ramp-oracle"
"${CXX:-clang++}" "${flags[@]}" -S "$root/tests/dsp_ramp_mixer.cpp" -o "$out/dsp-ramp-arm.s"
grep -q 'vmull.s16' "$out/dsp-ramp-arm.s"
grep -q 'vqadd.s16' "$out/dsp-ramp-arm.s"
printf 'PASS: Cortex-A9 ARM32 assembly contains widening multiply and saturating add\n'
