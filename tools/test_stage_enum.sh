#!/usr/bin/env bash
# precomp26 (GXV_STAGE_ENUM): host test of the pure parts (vgx_stage_enum.h), plain and ASan+UBSan, then ThreadSanitizer for the inbox.
#   bash tools/test_stage_enum.sh [out-dir]
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d "${TMPDIR:-/tmp}/stageenum.XXXXXX")}
mkdir -p "$out"
for san in "" "-fsanitize=address,undefined -fno-sanitize-recover=undefined" "-fsanitize=thread"; do
    "${CXX:-g++}" -std=gnu++17 -O1 -g -Wall -Wextra -pthread $san -I"$root/platform/vgx/src" "$root/tests/stage_enum_model.cpp" -o "$out/stage_enum_model"
    "$out/stage_enum_model"
done
