#!/usr/bin/env bash
# GXV_DL_KEEP_MIB's eviction choice (platform/vgx/src/vgx_dlkeep.h), on the host.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${OUT:-/tmp/vgx_dlkeep}
mkdir -p "$out"
cd "$root"
${CXX:-clang++} -std=c++17 -O1 -Wall -Wextra -Iplatform/vgx/src tests/vgx_dlkeep_test.cpp -o "$out/dlkeep"
"$out/dlkeep"
