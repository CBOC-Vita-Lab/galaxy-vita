#!/usr/bin/env bash
# endwait26: host test of GXV_ENDWAIT_LOG record and line (platform/vgx/src/vgx_endwait.h), plain and ASan+UBSan.  bash tools/test_endwait.sh [out-dir]
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d "${TMPDIR:-/tmp}/endwait.XXXXXX")}
mkdir -p "$out"
for san in "" "-fsanitize=address,undefined -fno-sanitize-recover=undefined"; do
    "${CXX:-g++}" -std=gnu++17 -O1 -g -Wall -Wextra $san "$root/tests/endwait_model.cpp" -o "$out/endwait_model"
    "$out/endwait_model"
done
