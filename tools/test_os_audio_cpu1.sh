#!/usr/bin/env bash
# GXV_AUDIO_CPU1 (platform/src/os/os_thread.cpp, port_audio_window): the real scheduler file, the SDK's OSMessage.c and OSMutex.c and real
# host threads (tests/os_audio_cpu1.cpp), built with g++ and ThreadSanitizer; the switch unset, 0 and 1, with the main thread busy all turn
# (as in the lit Observatory) and with time to sleep, and with GXV_AUDIO_HOLD 0 and 8.  Then the earlier scheduler tests (switch unset)
# under the same build.  Then the stage-load model (tests/os_audio_cpu1_load.cpp: a loader, a decompressor and helper threads with
# critical sections, mutexes and JAudio resource work while the main thread's windows open and close; it found the r60 hang): under
# ThreadSanitizer (switch 0, and 1 three times) and 20 times without it (faster, more interleavings; before the fix about 1 run in 6 hung).
# No console, no Vita timing.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
export TSAN_OPTIONS=halt_on_error=1
flags=(-std=c++20 -O1 -g -DTARGET_PC=1 -DNDEBUG=1 -fpermissive -w -D_Static_assert=static_assert -ffunction-sections -Wl,--gc-sections
       -include "$root/tests/glibc_fabs_shim.h" -I"$root/platform/include" -I"$root/decomp/libs/RVL_SDK/include" -include string
       -include "$root/platform/include/port/compat.h" -fsanitize=thread -pthread)
for t in os_audio_cpu1 os_audio_cpu1_load os_audio_hold_threads os_irq os_audio_hold; do
  "${CXX:-g++}" "${flags[@]}" "$root/tests/$t.cpp" -o "$out/$t"
done
for busy in 1 0; do
  for cpu1 in unset 0 1; do
    for hold in 8 0; do
      if [[ $cpu1 == unset ]]; then
        TEST_BUSY=$busy GXV_AUDIO_HOLD=$hold GXV_AUDIO_STATS=1 env -u GXV_AUDIO_CPU1 timeout 120 "$out/os_audio_cpu1" | tail -3
      else
        TEST_BUSY=$busy GXV_AUDIO_HOLD=$hold GXV_AUDIO_CPU1=$cpu1 timeout 120 "$out/os_audio_cpu1" | tail -3
      fi
    done
  done
done
for m in unset 0 8 12; do
  if [[ $m == unset ]]; then env -u GXV_AUDIO_HOLD timeout 60 "$out/os_audio_hold_threads"; else GXV_AUDIO_HOLD=$m timeout 60 "$out/os_audio_hold_threads"; fi
done
"$out/os_irq"
"$out/os_audio_hold"
for cpu1 in 0 1 1 1; do GXV_AUDIO_HOLD=8 GXV_AUDIO_CPU1=$cpu1 timeout 120 "$out/os_audio_cpu1_load" | tail -2; done
nosan=()
for f in "${flags[@]}"; do [[ $f == -fsanitize=thread ]] || nosan+=("$f"); done
"${CXX:-g++}" "${nosan[@]}" -O2 "$root/tests/os_audio_cpu1_load.cpp" -o "$out/os_audio_cpu1_load_fast"
for i in $(seq 1 20); do GXV_AUDIO_HOLD=8 GXV_AUDIO_CPU1=1 timeout 120 "$out/os_audio_cpu1_load_fast" | tail -1; done
echo "GXV_AUDIO_CPU1: all runs passed, ThreadSanitizer clean"
