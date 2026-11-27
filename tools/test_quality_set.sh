#!/bin/bash
# gfx26: host tests of GXV_QUALITY_SET: the pure table (platform/include/port/quality_set.h, tests/quality_set.cpp) and its launch glue
# (platform/src/settings/settings_launch.cpp, tests/quality_set_launch.cpp, one process per Picture mode). UBSan on the first.
set -u
root="$(cd "$(dirname "$0")/.." && pwd)"
out="${TMPDIR:-/tmp}/quality_set_test.$$"; mkdir -p "$out"
CXX="${CXX:-g++}"
"$CXX" -std=gnu++17 -O1 -g -Wall -Wextra -fsanitize=undefined -fno-sanitize-recover=undefined -I"$root/platform/include" "$root/tests/quality_set.cpp" -o "$out/quality_set" || { echo "build FAILED"; exit 2; }
"$out/quality_set" || exit 1
core=("$root/platform/src/settings/player_settings.cpp" "$root/platform/src/settings/settings_store.cpp" "$root/platform/src/input/control_bindings.cpp" "$root/platform/src/settings/settings_launch.cpp")
"$CXX" -std=c++17 -O1 -g -Wall -Wextra -Werror -I"$root/platform/include" "${core[@]}" "$root/tests/quality_set_launch.cpp" -o "$out/launch" || { echo "launch build FAILED"; exit 2; }
for m in quality locked performance env-quality env-performance off; do "$out/launch" "$m" || { echo "launch $m FAILED"; exit 1; }; done
rm -rf "$out"; echo "test_quality_set: ok"
