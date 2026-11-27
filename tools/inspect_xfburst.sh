#!/usr/bin/env bash
# Source-only object/assembly inspection; no game or device executable.
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
flags=(-O3 -DNDEBUG -fno-fast-math -ffp-contract=on -fno-strict-aliasing -fwrapv -fms-extensions
    -DTARGET_PC=1 -DPORT_PLATFORM_CODE=1 -DPORT_RUNTIME_DIAGNOSTICS=0 -DPORT_LOGGING=0
    -Iplatform/include -Iplatform/src/gx -Idecomp/libs/RVL_SDK/include
    -Idecomp/libs/JSystem/include -Idecomp/include -Wno-unknown-pragmas -Wno-header-guard)
for target in host host-noneon arm32 thumb2; do
    arch=()
    if test "$target" = arm32 || test "$target" = thumb2; then
        arch=(--target=arm-linux-gnueabihf --gcc-toolchain=/usr -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard)
    fi
    if test "$target" = thumb2; then arch+=(-mthumb -munaligned-access -O2); fi
    if test "$target" = host-noneon; then arch=(-U__ARM_NEON); fi
    for source in baseline current; do
        path="$out/baseline-GXTransform.c"
        if test "$source" = current; then path=decomp/src/RVL_SDK/gx/GXTransform.c; fi
        "${CC:-clang}" -std=c11 "${flags[@]}" "${arch[@]}" -S "$path" -o "$out/$target-$source-transform.s"
    done
    "${CXX:-clang++}" -std=c++20 "${flags[@]}" "${arch[@]}" -S platform/src/gx/gx_fifo.cpp -o "$out/$target-fifo.s"
done
sed '/^[[:space:]]*\.file[[:space:]]/d' "$out/host-noneon-baseline-transform.s" > "$out/noneon-baseline-normalized.s"
sed '/^[[:space:]]*\.file[[:space:]]/d' "$out/host-noneon-current-transform.s" > "$out/noneon-current-normalized.s"
cmp "$out/noneon-baseline-normalized.s" "$out/noneon-current-normalized.s"
printf 'PASS host/ARM -O3, Cortex-A9 Thumb2 -O2 emitted; non-NEON full GXTransform assembly identical\n'
