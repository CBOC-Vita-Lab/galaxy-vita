#!/usr/bin/env bash
# GXV_PACE24 (pace24): the steady-30 rule with the display's holds, on the model of the console's pipeline (tests/pace24_model.cpp), plus a
# syntax check of the Vita-only display hook (SmoothShown in platform/vgx/vita/vgx_host_galaxy_vita.cpp, extracted and compiled against stubs)
# and of perf.cpp's host path. The three existing steady-cadence suites must still pass (every default is today's).
#   tools/test_pace24.sh [output-dir]       exit status 0 when everything passes
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
cxx=${CXX:-g++}
"$cxx" -std=c++17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer -I"$root/platform/include" \
  "$root/tests/pace24_model.cpp" -o "$out/pace24_model"
UBSAN_OPTIONS=halt_on_error=1 "$out/pace24_model"
for t in steady_cadence_model steady_cadence_seamless; do
  "$cxx" -std=c++17 -O1 -Wall -Wextra -Werror -I"$root/platform/include" "$root/tests/$t.cpp" -o "$out/$t"
  "$out/$t" | tail -1
done
# the Vita-only display hook, extracted and compiled with stubs (no Vita SDK here)
{
  cat <<'STUB'
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include "port/frame_pace.h"
namespace vgx { long EnvInt(const char*, long d) { return d; } namespace backend { int QualityRung() { return 2; } } }
static uint64_t NowUs() { return 1; }
extern "C" void port_log_summary(const char* fmt, ...) __attribute__((weak));
// (stackgate31, 21 Nov: SmoothShown logs through port_log_summary_cb, the display-queue callback's line writer, not port_log_summary)
extern "C" void port_log_summary_cb(const char* fmt, ...) __attribute__((weak));
extern "C" int port_pace_steady30_engaged(void) __attribute__((weak));
extern "C" uint32_t port_pace_steady30_changes(void) __attribute__((weak));
STUB
  sed -n '/^void SmoothShown(uint32_t shown, bool fixed30) {/,/^}/p' "$root/platform/vgx/vita/vgx_host_galaxy_vita.cpp"
  echo 'int main() { SmoothShown(1, false); return 0; }'
} > "$out/smooth_hook.cpp"
"$cxx" -std=c++17 -Wall -Wextra -Werror -I"$root/platform/include" -fsyntax-only "$out/smooth_hook.cpp"
echo "vita display hook: syntax ok"
"$cxx" -std=c++17 -fsyntax-only -I"$root/platform/include" -I"$root/platform/src" -I"$root/platform" "$root/platform/src/port/perf.cpp"
echo "perf.cpp host path: syntax ok"
echo "PASS pace24"
