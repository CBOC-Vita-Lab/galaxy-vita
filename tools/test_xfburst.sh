#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
cd "$root"
if test -n "${XF_BURST_BASELINE_PATH:-}"; then
    cp "$XF_BURST_BASELINE_PATH" "$out/baseline-GXTransform.c"
else
    git show 392ee7009fe1152b5bd397e6398f00404303a264:decomp/src/RVL_SDK/gx/GXTransform.c > "$out/baseline-GXTransform.c"
fi
flags=(-O2 -g -fno-fast-math -ffp-contract=on -fno-strict-aliasing -DTARGET_PC=1 -DPORT_PLATFORM_CODE=1
    -DPORT_GX_VGX_ONLY=1 -fms-extensions -Iplatform/include -Iplatform/src/gx
    -Idecomp/libs/RVL_SDK/include -Idecomp/libs/JSystem/include -Idecomp/include
    -Wno-register -Wno-c++11-narrowing -Wno-format -Wno-unknown-pragmas -ffunction-sections -fdata-sections)
link=(-Wl,--gc-sections)
if test "$(uname -s)" = Darwin; then link=(-Wl,-dead_strip); fi
sanitize=(-fsanitize=address,undefined)
runner=(env)
if test "${XF_BURST_ARM32:-0}" = 1; then
    flags+=(--target=arm-linux-gnueabihf --gcc-toolchain=/usr -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -mthumb -munaligned-access)
    sanitize=()
    runner=("${QEMU_ARM:-qemu-arm}" -L "${ARM_SYSROOT:-/usr/arm-linux-gnueabihf}")
fi
if test "${XF_BURST_NO_NEON:-0}" = 1; then flags+=(-U__ARM_NEON); fi
for mode in diagnostic quiet; do
    define=(-DPORT_RUNTIME_DIAGNOSTICS=1 -DPORT_LOGGING=1)
    if test "$mode" = quiet; then define=(-DPORT_RUNTIME_DIAGNOSTICS=0 -DPORT_LOGGING=0); fi
    "${CC:-clang}" -std=c11 "${flags[@]}" "${define[@]}" "${sanitize[@]}" -c tests/xfburst_driver.c -o "$out/driver-$mode.o"
    # The C API historically accepts other integer type values. Its pinned C
    # source is namespaced in the C++ oracle; disable only enum-range checking.
    "${CXX:-clang++}" -std=c++20 "${flags[@]}" "${define[@]}" "${sanitize[@]}" -fno-sanitize=enum \
        "-DXF_BURST_BASELINE=\"$out/baseline-GXTransform.c\"" tests/xfburst.cpp "$out/driver-$mode.o" \
        "${link[@]}" -pthread -o "$out/test-$mode"
    for setting in 1 unset 0 2 -1 invalid 9999999999999999999999999999999999 0x1 1suffix; do
        "${runner[@]}" "$out/test-$mode" "$setting" parity
    done
    "${runner[@]}" "$out/test-$mode" 1 fast-fault
    if test "$mode" = diagnostic; then "${runner[@]}" "$out/test-$mode" 2 fault; fi
done
