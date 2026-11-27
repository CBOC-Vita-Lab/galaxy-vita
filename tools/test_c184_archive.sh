#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
sed -n '/^namespace {$/,$p' decomp/src/Game/System/FileRipper.cpp > "$tmp/ripper.inc"
"${CXX:-c++}" -std=c++17 -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
    -I"$tmp" tests/c184_archive.cpp -o "$tmp/check"
"$tmp/check" "$@"
GXV_ARCHIVE_BATCH=0 "$tmp/check"
