#!/usr/bin/env bash
# Host-side test of GXV_AUTOQ_ANISO (Config::aniso in platform/vgx/src/vgx_autoq.cpp: anisotropic filtering as the top step of the
# quality ladder, on last when room returns and given up first under load). Synthetic traces; with the option off the decisions hash to
# the controller's golden digests. Exit status 0 when every check passes.
#   tools/test_vgx_autoq_aniso.sh [output-dir] [verbose]      (default: a temporary directory)
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
cxx=${CXX:-g++}
nice -n 10 "$cxx" -std=c++17 -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-omit-frame-pointer \
  "$root/tests/vgx_autoq_aniso.cpp" "$root/platform/vgx/src/vgx_autoq.cpp" -o "$out/vgx_autoq_aniso"
UBSAN_OPTIONS=halt_on_error=1 "$out/vgx_autoq_aniso" "${2:-}"
