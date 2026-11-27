#!/usr/bin/env bash
# GXV_LATENCY24 (latency24): the evidence-based present-latency rule on the pacer model with log-derived scenes (tests/latency24_model.cpp: identity with f6c0c23's
# PresentSync with the switch off, unit checks, "no more late pictures than today, lower latency"), the existing present-sync / steady-cadence suites, and syntax
# checks of the Vita-only code (extracted and compiled against stubs; no Vita SDK here), of wpad.cpp (host build) and perf.cpp.
#   tools/test_latency24.sh [output-dir]       exit status 0 when everything passes
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
cxx=${CXX:-g++}
host="$root/platform/vgx/vita/vgx_host_galaxy_vita.cpp"
"$cxx" -std=c++17 -O1 -g -Wall -Wextra -Werror -fsanitize=undefined -fno-omit-frame-pointer -I"$root/platform/include" -I"$root/tests" \
  "$root/tests/latency24_model.cpp" -o "$out/latency24_model"
UBSAN_OPTIONS=halt_on_error=1 "$out/latency24_model"
"$root/tools/test_present_sync_model.sh" "$out/ps" | tail -1
for t in steady_cadence_model steady_cadence_seamless; do
  "$cxx" -std=c++17 -O1 -Wall -Wextra -Werror -I"$root/platform/include" "$root/tests/$t.cpp" -o "$out/$t"
  "$out/$t" | tail -1
done
# the Vita-only census (display callback side, game-thread note) extracted with stubs
{
  cat <<'STUB'
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstdarg>
#include "port/frame_pace.h"
static void EnvLong(const char* name, long& value, long lo, long hi) {
    if (const char* e = std::getenv(name); e && *e) value = std::atol(e);
    value = std::min(std::max(value, lo), hi);
}
extern "C" void port_log_summary(const char* fmt, ...) __attribute__((weak));
extern "C" unsigned port_vgx_queue_max_take(void) __attribute__((weak));
extern "C" void port_input_age_take(unsigned*, double*, double*, unsigned*) __attribute__((weak));
static bool g_syncOn = true;
STUB
  sed -n '/^struct TurnNote {/,/^\/\/ GXV_PRESENT_SYNC (frame_pace.h PresentSync): the display/p' "$host" | sed '$d'
  sed -n '/^extern "C" void port_vgx_vita_note_turn(/,/^}/p' "$host"
  cat <<'STUB'
int main() {
    port::pace::PresentSync sync;
    port::pace::PresentSync::Stats st;
    port_vgx_vita_note_turn(5, 100, 1000000000ll, 990000000ll);
    LatencyShown(5, 103);
    LatencyBlock(st, sync, 1);
    return 0;
}
STUB
} > "$out/census_hook.cpp"
"$cxx" -std=c++17 -Wall -Wextra -Werror -I"$root/platform/include" "$out/census_hook.cpp" -o "$out/census_hook"
"$out/census_hook"
echo "vita latency census: compiles and runs on stubs"
# the SyncDisplay wiring (the Latency24 config lambda part and the entry bookkeeping must at least parse in place: the whole file needs the Vita SDK, so only the
# diff-relevant lines are checked for the names they use)
for n in LatencyShown LatencyBlock NoteBacklog ConfigureLatency24 port_vgx_vita_note_turn; do
  grep -q "$n" "$host" "$root/platform/include/port/frame_pace.h" || { echo "missing $n"; exit 1; }
done
# wpad.cpp on the host, perf.cpp
"${CLANGXX:-clang++}" -std=c++20 -fsyntax-only -fno-short-enums -DPORT_RUNTIME_DIAGNOSTICS=0 -DTARGET_PC -include "$root/tests/glibc_fabs_shim.h" \
  -I"$root/platform/include" -I"$root/vita/runtime" -I"$root/decomp/libs/RVL_SDK/include" "$root/platform/src/input/wpad.cpp"
echo "wpad.cpp host path: syntax ok"
"$cxx" -std=c++17 -fsyntax-only -I"$root/platform/include" -I"$root/platform/src" -I"$root/platform" "$root/platform/src/port/perf.cpp"
echo "perf.cpp host path: syntax ok"
echo "PASS latency24"
