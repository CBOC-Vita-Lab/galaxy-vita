#!/usr/bin/env bash
# Host-side test of the shed order (GXV_AUTOQ_SHED, Config::shed): traces shaped like Melty Molten, Deep Dark and Honeyhive at 960x544, the way back,
# a lying prediction, the stress cycle. ASan + UBSan. Exit status 0 when every check passes.
#   tools/test_vgx_autoq_shed.sh [output-dir]      (VERBOSE=1: every change of every trace)
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
cxx=${CXX:-g++}
nice -n 10 "$cxx" -std=c++17 -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-omit-frame-pointer \
  "$root/tests/vgx_autoq_shed.cpp" "$root/platform/vgx/src/vgx_autoq.cpp" -o "$out/vgx_autoq_shed"
UBSAN_OPTIONS=halt_on_error=1 "$out/vgx_autoq_shed" ${VERBOSE:+verbose}
