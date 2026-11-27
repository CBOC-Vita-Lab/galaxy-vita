#!/usr/bin/env bash
# bootscr26: host test of vgx_boot_screen.h (the boot screen's end and the port-picture guard), plain and ASan+UBSan.
#   bash tools/test_boot_screen.sh [out-dir]
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d "${TMPDIR:-/tmp}/bootscreen.XXXXXX")}
mkdir -p "$out"
for san in "" "-fsanitize=address,undefined -fno-sanitize-recover=undefined"; do
    "${CXX:-g++}" -std=gnu++17 -O1 -g -Wall -Wextra $san "$root/tests/boot_screen_model.cpp" -o "$out/boot_screen_model"
    "$out/boot_screen_model"
done
