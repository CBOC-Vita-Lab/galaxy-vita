#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
cd "$root"
git show 392ee7009fe1152b5bd397e6398f00404303a264:decomp/src/RVL_SDK/gx/GXTransform.c > "$out/baseline.c"
flags=(-O3 -DNDEBUG -fno-fast-math -ffp-contract=on -fno-strict-aliasing -fwrapv -fms-extensions
    -DTARGET_PC=1 -DPORT_PLATFORM_CODE=1 -DPORT_GX_VGX_ONLY=1 -DPORT_RUNTIME_DIAGNOSTICS=0 -DPORT_LOGGING=0
    -Iplatform/include -Iplatform/src/gx -Idecomp/libs/RVL_SDK/include
    -Idecomp/libs/JSystem/include -Idecomp/include -Wno-unknown-pragmas -Wno-header-guard
    -ffunction-sections -fdata-sections)
link=(-Wl,--gc-sections)
if test "$(uname -s)" = Darwin; then link=(-Wl,-dead_strip); fi
for version in baseline current; do
    source="$out/baseline.c"
    if test "$version" = current; then source=decomp/src/RVL_SDK/gx/GXTransform.c; fi
    "${CC:-clang}" -std=c11 "${flags[@]}" -c "$source" -o "$out/$version.o"
    "${CXX:-clang++}" -std=c++20 "${flags[@]}" tests/xfburst_bench.cpp "$out/$version.o" "${link[@]}" -o "$out/$version"
done
for order in 'baseline:0 current:0 current:1' 'current:1 current:0 baseline:0' 'current:0 baseline:0 current:1' \
             'current:1 baseline:0 current:0' 'baseline:0 current:1 current:0'; do
    for item in $order; do
        printf '%s ' "$item"
        "$out/${item%:*}" "${item#*:}" 1000000
    done
done
