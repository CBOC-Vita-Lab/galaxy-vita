#!/usr/bin/env bash
# GXV_HOT_KNOBS (platform/include/port/hot_knob.h): PORT_HOT_KNOB's values, read-once, "unset"-pattern and racing-first-read behaviour
# (tests/hot_knob.cpp) under ASan/UBSan and ThreadSanitizer, with the switch unset, 1, empty, 0 and 0x0; then a compile of the test for
# the console's target (clang, arm-vita-eabi) when the Vita SDK is installed, checking that a knob read has no `dmb` on its fast path.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d "${TMPDIR:-/tmp}/hot-knob.XXXXXX")}
mkdir -p "$out"
export ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 TSAN_OPTIONS=halt_on_error=1
for san in address,undefined thread; do
    # (-Wno-tsan: g++ notes that ThreadSanitizer does not model the GXV_HOT_KNOBS=0 path's acquire fence; nothing here relies on it)
    "${CXX:-g++}" -std=c++20 -O2 -g -Wall -Wextra -Werror -Wno-tsan -pthread "-fsanitize=$san" -I"$root/platform/include" \
        "$root/tests/hot_knob.cpp" -o "$out/hot-knob-${san%%,*}"
    env -u GXV_HOT_KNOBS "$out/hot-knob-${san%%,*}" 0
    GXV_HOT_KNOBS=1 "$out/hot-knob-${san%%,*}" 0
    GXV_HOT_KNOBS= "$out/hot-knob-${san%%,*}" 0
    GXV_HOT_KNOBS=0 "$out/hot-knob-${san%%,*}" 1
    GXV_HOT_KNOBS=0x0 "$out/hot-knob-${san%%,*}" 1
done
sdk=${VITASDK:-/opt/vitasdk}
if command -v clang++ >/dev/null && test -d "$sdk/arm-vita-eabi/include/c++"; then
    cxxinc=$(ls -d "$sdk"/arm-vita-eabi/include/c++/* | head -1)
    clang++ --target=arm-vita-eabi --sysroot="$sdk/arm-vita-eabi" -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -mthumb -femulated-tls \
        -fno-exceptions -fno-rtti -nostdlibinc -nostdinc++ -isystem "$cxxinc" -isystem "$cxxinc/arm-vita-eabi" -isystem "$sdk/arm-vita-eabi/include" \
        -std=gnu++20 -O2 -I"$root/platform/include" -c "$root/tests/hot_knob.cpp" -o "$out/hot-knob-vita.o"
    # KnobInt's fast path: one load, one compare; its only barrier is on the GXV_HOT_KNOBS=0 path (after the second word's load).
    if command -v llvm-objdump >/dev/null; then
        llvm-objdump -d --no-show-raw-insn "$out/hot-knob-vita.o" > "$out/hot-knob-vita.dis"
        if grep -q "guard variable\|__cxa_guard" <(llvm-objdump -d -r -C "$out/hot-knob-vita.o" | grep -B2 -A2 "KnobInt\b" || true); then
            echo "FAIL: a guard in KnobInt"; exit 1
        fi
    fi
    echo "hot knob: the console build compiles"
fi
echo "hot knob: PASS"
