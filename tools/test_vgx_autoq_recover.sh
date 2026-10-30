#!/usr/bin/env bash
# Host-side test of GXV_AUTOQ_RECOVER (Config::recover in platform/vgx/src/vgx_autoq.cpp): with the option off the controller's
# decisions are those it made before the option (seeded traces, hashed); with it on, the way back up after an overload is
# quicker and not held down by single frames, and nothing steps down earlier or further. Exit status 0 when every check passes.
#   tools/test_vgx_autoq_recover.sh [output-dir]      (default: a temporary directory)
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
cxx=${CXX:-g++}
nice -n 10 "$cxx" -std=c++17 -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-omit-frame-pointer \
  "$root/tests/vgx_autoq_recover.cpp" "$root/platform/vgx/src/vgx_autoq.cpp" -o "$out/vgx_autoq_recover"
UBSAN_OPTIONS=halt_on_error=1 "$out/vgx_autoq_recover"
