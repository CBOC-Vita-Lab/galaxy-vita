#!/usr/bin/env bash
# GXV_DSPRESAMPLE / GXV_DSPAFC (ported from Producdevity's fork): tests/dsp_voice_input.cpp, the resampler and AFC decoder in every
# mode against copies of the original loops (20,000 cases each), then the whole mixer: one digest in every mode. Host build (on x86
# the resampler's scalar form; the NEON form is checked for its widening instructions in an ARM32 assembly listing).
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d "${TMPDIR:-/tmp}/dsp-voice.XXXXXX")}
mkdir -p "$out"
flags=(-std=c++20 -O2 -fwrapv -fms-extensions -DTARGET_PC=1 -DPORT_PLATFORM_CODE=1 -DPORT_RUNTIME_DIAGNOSTICS=0
    -I"$root/platform/include" -I"$root/decomp/include" -I"$root/decomp/libs/JSystem/include" -I"$root/decomp/libs/RVL_SDK/include"
    -I"$root/decomp/libs/nw4r/include" -I"$root/decomp/libs/RVLFaceLib/include"
    -include string -include "$root/platform/include/port/compat.h" -Wno-register -Wno-unknown-pragmas)
export ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1
"${CXX:-clang++}" "${flags[@]}" -g -fsanitize=address,undefined -fno-omit-frame-pointer "$root/tests/dsp_voice_input.cpp" -o "$out/voice"
digests=()
for modes in "0 0" "1 1" "2 2" "1 0" "0 1" "2 1" "3 garbage"; do
    set -- $modes
    line=$(GXV_DSPRESAMPLE=$1 GXV_DSPAFC=$2 "$out/voice")
    echo "$line"
    digests+=("$(echo "$line" | cut -d" " -f2)")
done
test "$(printf "%s\n" "${digests[@]}" | sort -u | wc -l)" -eq 1
"${CXX:-clang++}" --target=arm-linux-gnueabihf --gcc-toolchain=/usr -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard "${flags[@]}" \
    -S "$root/tests/dsp_voice_input.cpp" -o "$out/voice-arm.s"
grep -Eq 'vmull\.s16' "$out/voice-arm.s" && grep -Eq 'vqshrn\.s64' "$out/voice-arm.s" || { echo "FAIL: no NEON resampler in the ARM32 listing"; exit 1; }
echo "PASS: one mixer digest in every mode; resampler and AFC decoder exact; NEON resampler compiled for ARM32"
