#!/usr/bin/env bash
# GXV_DRAWDONE_OTHER (platform/include/port/drawdone_other.h): the loading thread's draw-done wait. A deterministic model of the guest
# CPU, the one-bit DrawDone flag and the game thread's turns (with the real port::drawdone::Deferral) shows the 2.2 / r110 shared wait
# starving the loading thread at 60 pictures a second (issue 3: no way out of the title in Performance mode) and the fix getting
# through in every mode, cadence and finish pattern; plus a compile check of the two files that carry it.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d "${TMPDIR:-/tmp}/drawdone-other.XXXXXX")}
mkdir -p "$out"
export ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1
"${CXX:-g++}" -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -I"$root/platform/include" \
    "$root/tests/drawdone_other.cpp" -o "$out/drawdone-other"
"$out/drawdone-other"
inc=(-DTARGET_PC=1 -DPORT_PLATFORM_CODE=1 -I"$root/platform/include" -I"$root/decomp/include" -I"$root/decomp/libs/JSystem/include"
    -I"$root/decomp/libs/RVL_SDK/include" -I"$root/decomp/libs/nw4r/include" -I"$root/decomp/libs/RVLFaceLib/include")
"${CC:-clang}" -std=gnu11 -fsyntax-only -fms-extensions -Wno-everything "${inc[@]}" "$root/decomp/src/RVL_SDK/gx/GXMisc.c"
"${CXX_SYNTAX:-clang++}" -std=c++20 -fsyntax-only -fms-extensions -fwrapv -Wno-everything -include string \
    -include "$root/platform/include/port/compat.h" "${inc[@]}" "$root/decomp/src/Game/System/MainLoopFramework.cpp"
echo "drawdone other: model clean under ASan/UBSan; GXMisc.c and MainLoopFramework.cpp compile"
