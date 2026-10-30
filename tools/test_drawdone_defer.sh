#!/usr/bin/env bash
# GXV_DRAWDONE_DEFER: the state machine behind the deferred end-of-turn wait (platform/include/port/drawdone_defer.h), checked against a
# model of the one-bit DrawDone flag and a front end that finishes at random moments (address+UB sanitizers, then ThreadSanitizer),
# plus a compile check of the three decomp files that carry the hooks.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d "${TMPDIR:-/tmp}/drawdone-defer.XXXXXX")}
mkdir -p "$out"
export ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 TSAN_OPTIONS=halt_on_error=1
for san in address,undefined thread; do
    "${CXX:-clang++}" -std=c++20 -O1 -g -Wall -Wextra -pthread "-fsanitize=$san" -I"$root/platform/include" \
        "$root/tests/drawdone_defer.cpp" -o "$out/drawdone-${san%%,*}"
    "$out/drawdone-${san%%,*}"
done
for diag in 0 1; do
    "${CXX:-clang++}" -std=c++20 -fsyntax-only -fms-extensions -Wall -DPORT_RUNTIME_DIAGNOSTICS=$diag -DTARGET_PC=1 -DPORT_PLATFORM_CODE=1 \
        -include string -include "$root/platform/include/port/compat.h" -I"$root/platform/include" -I"$root/decomp/include" \
        -I"$root/decomp/libs/RVL_SDK/include" "$root/platform/src/port/drawdone_defer.cpp"
done
# The hooks: GXMisc.c as C, the two game files as C++ (syntax only; the port headers decide the rest).
inc=(-DTARGET_PC=1 -DPORT_PLATFORM_CODE=1 -I"$root/platform/include" -I"$root/decomp/include" -I"$root/decomp/libs/JSystem/include"
    -I"$root/decomp/libs/RVL_SDK/include" -I"$root/decomp/libs/nw4r/include" -I"$root/decomp/libs/RVLFaceLib/include")
"${CC:-clang}" -std=gnu11 -fsyntax-only -fms-extensions -Wno-everything "${inc[@]}" "$root/decomp/src/RVL_SDK/gx/GXMisc.c"
for f in Game/System/MainLoopFramework.cpp Game/System/GameSystem.cpp; do
    "${CXX:-clang++}" -std=c++20 -fsyntax-only -fms-extensions -fwrapv -Wno-everything -include string \
        -include "$root/platform/include/port/compat.h" "${inc[@]}" "$root/decomp/src/$f"
done
echo "drawdone defer: state machine and model clean under ASan/UBSan and TSan; hook files compile"
