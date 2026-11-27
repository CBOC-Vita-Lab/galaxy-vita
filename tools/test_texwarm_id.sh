#!/usr/bin/env bash
# texwarm30: host test of the identity key, the v2 book, the candidate index and the scheduler (platform/vgx/src/vgx_texwarm_id.h), plain and ASan+UBSan.  bash tools/test_texwarm_id.sh [out-dir]
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d "${TMPDIR:-/tmp}/texwarm_id.XXXXXX")}
mkdir -p "$out"
for san in "" "-fsanitize=address,undefined -fno-sanitize-recover=undefined"; do
    "${CXX:-g++}" -std=gnu++17 -O1 -g -Wall -Wextra $san "$root/tests/texwarm_id_model.cpp" -o "$out/texwarm_id_model"
    "$out/texwarm_id_model"
done
