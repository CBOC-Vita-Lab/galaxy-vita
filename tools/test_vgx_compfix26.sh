#!/bin/bash
# compfix26 host test: why the effects compositor routed nothing at Freezeflame (near rule) and the fix; GXV_FXLOWRES_FORCE. Pure logic, no GL.
set -eo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${OUT:-/tmp/compfix26_test_build}; mkdir -p "$out"
nice -n 10 ${CXX:-c++} -std=c++17 -O1 -Wall -Wextra -I"$root/platform/vgx/src" "$root/tests/vgx_compfix26_test.cpp" -o "$out/vgx_compfix26_test"
"$out/vgx_compfix26_test"
nice -n 10 ${CXX:-c++} -std=c++17 -O1 -Wall -Wextra -I"$root/platform/vgx/src" "$root/tests/vgx_fxauto_test.cpp" -o "$out/vgx_fxauto_test"
"$out/vgx_fxauto_test" | tail -2
