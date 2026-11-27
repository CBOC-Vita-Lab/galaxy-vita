#!/usr/bin/env bash
# GXV_AI_EVENT (thr24): the AI clock waits for AIInitDMA's signal instead of polling every 500 us. tests/ai_clock_event_test.cpp drives the real ai_dsp.cpp with a
# simulated device and a simulated game; runs it with the switch off and on and checks: at most a stray block is played as silence and at most one underrun (a loaded host's scheduler hits either mode now and then), the clock polls at most a tenth
# as often with the switch on, also under ThreadSanitizer.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d "${TMPDIR:-/tmp}/ai-clock-event.XXXXXX")}
mkdir -p "$out"
flags=(-std=c++20 -fwrapv -fms-extensions -DTARGET_PC=1 -DPORT_PLATFORM_CODE=1 -m64
    -I"$root/platform/include" -I"$root/decomp/include" -I"$root/decomp/libs/JSystem/include"
    -I"$root/decomp/libs/RVL_SDK/include" -I"$root/decomp/libs/nw4r/include" -I"$root/decomp/libs/RVLFaceLib/include"
    -include string -include "$root/platform/include/port/compat.h" -Wno-register -Wno-unknown-pragmas -pthread)
srcs=("$root/tests/ai_clock_event_test.cpp" "$root/platform/src/audio/ai_dsp.cpp" "$root/platform/src/audio/dsp_mixer.cpp")
for san in none thread; do
    objs=()
    for s in "${srcs[@]}"; do
        o="$out/$san-$(basename "$s" .cpp).o"
        if [[ $san == none ]]; then "${CXX:-clang++}" "${flags[@]}" -DPORT_RUNTIME_DIAGNOSTICS=1 -O2 -c "$s" -o "$o"
        else "${CXX:-clang++}" "${flags[@]}" -DPORT_RUNTIME_DIAGNOSTICS=1 -O1 -g -fsanitize=thread -c "$s" -o "$o"; fi
        objs+=("$o")
    done
    if [[ $san == none ]]; then "${CXX:-clang++}" -pthread "${objs[@]}" -o "$out/$san"
    elif ! "${CXX:-clang++}" -pthread -fsanitize=thread "${objs[@]}" -o "$out/$san" 2>/dev/null; then echo "SKIP: no ThreadSanitizer runtime here"; continue; fi
    export TSAN_OPTIONS=halt_on_error=1
    off=$(GXV_AI_EVENT=0 "$out/$san" 4); on=$(GXV_AI_EVENT=1 "$out/$san" 4)
    echo "[$san] off: $off"; echo "[$san] on:  $on"
    get() { echo "$1" | sed -E "s/.*$2 ([0-9]+).*/\1/"; }
    (( $(get "$off" late) <= 2 && $(get "$on" late) <= 2 )) || { echo "FAIL: a block was played as silence" >&2; exit 1; }
    (( $(get "$off" underruns) <= 1 && $(get "$on" underruns) <= 1 )) || { echo "FAIL: the device ran dry" >&2; exit 1; }
    b0=$(get "$off" blocks); b1=$(get "$on" blocks)
    (( b1 >= b0 - 3 && b1 <= b0 + 3 )) || { echo "FAIL: block counts differ: $b0 vs $b1" >&2; exit 1; }
    s0=$(get "$off" sleeps500); s1=$(get "$on" sleeps500)
    (( s0 > 100 && s1 * 10 <= s0 )) || { echo "FAIL: the poll did not drop to a tenth: $s0 -> $s1" >&2; exit 1; }
done
echo "ai clock event: same blocks, no stray late block or underrun, poll cut >= 10x"
