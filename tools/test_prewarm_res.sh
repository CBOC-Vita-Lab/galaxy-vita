#!/usr/bin/env bash
# handover26: host test of GXV_PREWARM_RES's record / replay logic (platform/vgx/src/vgx_prewarm_res.h), plain and ASan+UBSan.  bash tools/test_prewarm_res.sh [out-dir]
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d "${TMPDIR:-/tmp}/prewarmres.XXXXXX")}
mkdir -p "$out"
for san in "" "-fsanitize=address,undefined -fno-sanitize-recover=undefined"; do
    "${CXX:-g++}" -std=gnu++17 -O1 -g -Wall -Wextra $san "$root/tests/prewarm_res_model.cpp" -o "$out/prewarm_res_model"
    "$out/prewarm_res_model"
done
