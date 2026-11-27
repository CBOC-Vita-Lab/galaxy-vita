#!/usr/bin/env bash
# getenv25 (platform/include/port/getenv25.h): the per-pass / per-upload / per-fresh-draw environment reads are made once and answer what
# vgx::EnvInt answered: unset, empty, 0, 1, a hex value, a negative and an overflowing value (tests/getenv25.cpp counts getenv calls).
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d "${TMPDIR:-/tmp}/getenv25.XXXXXX")}
mkdir -p "$out"
"${CXX:-g++}" -std=c++20 -O1 -g -Wall -Wextra -Werror -I"$root/platform/include" "$root/tests/getenv25.cpp" -o "$out/getenv25"
# (one process per value: each knob keeps its first answer)
for v in - '' 0 1 0x40 -5 99999999999 abc; do "$out/getenv25" "$v"; done
echo "getenv25: PASS"
