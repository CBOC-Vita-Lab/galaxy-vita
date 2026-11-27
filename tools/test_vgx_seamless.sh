#!/usr/bin/env bash
# seamless23 (GXV_SEAMLESS): quality transitions without a jump or a wrong picture. Three host tests:
#   tests/vgx_seamless.cpp           the back end as for the console (__VGX_VITA__, the GL stub): a step up held for its pieces and made
#                                    with none made in play, the kept objects of a size left, a texture kept until its rewrite, the stale
#                                    read watch, the transition log, the edge fade, the half-size effects' learned targets; and "off"
#   tests/vgx_autoq_seamless.cpp     the controller's cut gate (Config::seam)
#   tests/steady_cadence_seamless.cpp the steady 30's release at a cut and the quick re-engage (frame_pace.h)
#   tools/test_vgx_seamless.sh [output-dir]     exit status 0 when every check passes
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
cxx=${CXX:-g++}
san=${SAN--fsanitize=address,undefined}
status=0
nice -n 10 "$cxx" -std=c++17 -O1 -g -DNDEBUG -D__VGX_VITA__=1 $san -I"$root/armhf/glstub" -I"$root/tests/autoq_clock_stubs" -I"$root/platform/include" \
  -I"$root/platform/vgx/src" -I"$root/platform/vgx/third_party" "$root/tests/vgx_seamless.cpp" "$root/platform/vgx/src/vgx_shadergen.cpp" \
  "$root/platform/vgx/src/vgx_autoq.cpp" -o "$out/vgx_seamless" -lpthread
nice -n 10 "$cxx" -std=c++17 -O1 -g -Wall -Wextra $san "$root/tests/vgx_autoq_seamless.cpp" "$root/platform/vgx/src/vgx_autoq.cpp" -o "$out/vgx_autoq_seamless"
nice -n 10 "$cxx" -std=c++17 -O1 -g -Wall -Wextra -Werror $san "$root/tests/steady_cadence_seamless.cpp" -o "$out/steady_cadence_seamless"
export ASAN_OPTIONS=${ASAN_OPTIONS:-detect_leaks=0} UBSAN_OPTIONS=halt_on_error=1
echo "== back end, GXV_SEAMLESS=1";        "$out/vgx_seamless"        || status=1
echo "== back end, GXV_SEAMLESS=0 (2.3)";  "$out/vgx_seamless" off    || status=1
echo "== controller, Config::seam";        "$out/vgx_autoq_seamless"  || status=1
echo "== steady 30, cutRelease / quickTurns"; "$out/steady_cadence_seamless" || status=1
exit $status
