#!/usr/bin/env bash
# GXV_AUDIO_HOLD (platform/src/os/os_thread.cpp): the real scheduler file with the test's clock (tests/os_audio_hold.cpp), with real
# guest threads (tests/os_audio_hold_threads.cpp: switch unset, 0, 8, 12), and the original IRQ scheduler test (tests/os_irq.cpp) with
# the switch off. No console, no Vita timing.
# SAN=address,undefined adds sanitizers where the compiler has their runtimes.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
san=()
[[ -n ${SAN:-} ]] && san=(-fsanitize="$SAN" -fno-omit-frame-pointer)
for t in os_audio_hold os_audio_hold_threads os_irq; do
  "${CXX:-clang++}" -std=c++20 -O1 -g -DTARGET_PC=1 -DNDEBUG=1 -fms-extensions -ffunction-sections -Wl,--gc-sections \
    -I"$root/platform/include" -I"$root/decomp/libs/RVL_SDK/include" -include string -include "$root/platform/include/port/compat.h" \
    -Wno-register -Wno-unknown-pragmas -Wno-deprecated-volatile "${san[@]}" "$root/tests/$t.cpp" -o "$out/$t" -pthread
  if [[ $t == os_audio_hold_threads ]]; then
    for m in unset 0 8 12; do
      if [[ $m == unset ]]; then env -u GXV_AUDIO_HOLD timeout 60 "$out/$t"; else GXV_AUDIO_HOLD=$m timeout 60 "$out/$t"; fi
    done
  else
    UBSAN_OPTIONS=halt_on_error=1 "$out/$t"
  fi
done
