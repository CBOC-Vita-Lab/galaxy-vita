#!/usr/bin/env bash
# Host-side test of the stage-settable shader-key list (platform/vgx/src/vgx_fxkeys.h).  tools/test_vgx_fxkeys.sh [output-dir]
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}; mkdir -p "$out"
${CXX:-g++} -std=c++17 -O1 -Wall -Wextra -I"$root/platform/vgx/src" "$root/tests/vgx_fxkeys_test.cpp" -o "$out/vgx_fxkeys_test"
"$out/vgx_fxkeys_test"
