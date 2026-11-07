#!/usr/bin/env bash
# Host-side test of GXV_AUTOQ_FXSTEP (Config::fxStep in platform/vgx/src/vgx_autoq.cpp: half-size effects and blur before the
# 480x340 emergency rung, back off first when room returns) and of the floor census behind the basic log's "floor:" line.
# Synthetic traces; with the option off the decisions hash to the controller's golden digests. Exit status 0 when every check passes.
#   tools/test_vgx_autoq_fxstep.sh [output-dir] [verbose]      (default: a temporary directory)
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
cxx=${CXX:-g++}
nice -n 10 "$cxx" -std=c++17 -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-omit-frame-pointer \
  "$root/tests/vgx_autoq_fxstep.cpp" "$root/platform/vgx/src/vgx_autoq.cpp" -o "$out/vgx_autoq_fxstep"
UBSAN_OPTIONS=halt_on_error=1 "$out/vgx_autoq_fxstep" "${2:-}"
