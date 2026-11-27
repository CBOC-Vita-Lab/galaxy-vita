#!/usr/bin/env bash
# hitch25 (second part): host test of vgx_stage_room.h (GXV_STAGEQ_ROOM's victim order), plain and ASan+UBSan.  bash tools/test_stageroom.sh [out-dir]
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d "${TMPDIR:-/tmp}/stageroom.XXXXXX")}
mkdir -p "$out"
for san in "" "-fsanitize=address,undefined -fno-sanitize-recover=undefined"; do
    "${CXX:-g++}" -std=gnu++17 -O1 -g -Wall -Wextra $san "$root/tests/stageroom_model.cpp" -o "$out/stageroom_model"
    "$out/stageroom_model"
done
