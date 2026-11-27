#!/usr/bin/env bash
# dive27: host test of the rehearsal registry / runner (platform/include/port/rehearse.h), plain and ASan+UBSan.  bash tools/test_rehearse.sh [out-dir]
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d "${TMPDIR:-/tmp}/rehearse.XXXXXX")}
mkdir -p "$out"
for san in "" "-fsanitize=address,undefined -fno-sanitize-recover=undefined"; do
    "${CXX:-g++}" -std=gnu++17 -O1 -g -Wall -Wextra $san -I"$root/platform/include" "$root/tests/rehearse_model.cpp" -o "$out/rehearse_model"
    "$out/rehearse_model"
done
