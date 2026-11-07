#!/usr/bin/env bash
# Host-side test of the play-time quality options of the automatic quality controller (vgx_autoq.h "Play-time quality":
# GXV_AUTOQ_FREE, GXV_AUTOQ_WIPE, GXV_AUTOQ_LATEROOM, GXV_AUTOQ_ANISO_RUNG). Synthetic traces; with the options off the decisions hash
# to the controller's golden digests. Exit status 0 when every check passes.
#   tools/test_vgx_autoq_play.sh [output-dir] [verbose]      (default: a temporary directory)
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
cxx=${CXX:-g++}
nice -n 10 "$cxx" -std=c++17 -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-omit-frame-pointer \
  "$root/tests/vgx_autoq_play.cpp" "$root/platform/vgx/src/vgx_autoq.cpp" -o "$out/vgx_autoq_play"
UBSAN_OPTIONS=halt_on_error=1 "$out/vgx_autoq_play" "${2:-}"
