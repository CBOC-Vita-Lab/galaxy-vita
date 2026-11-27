#!/usr/bin/env bash
# GXV_LATENCY24_FAST (thr24): the shorter hold-off after a failed lowering on the pace33 pipeline model; off must equal the shipped rule exactly.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d "${TMPDIR:-/tmp}/l24fast.XXXXXX")}
mkdir -p "$out"
"${CXX:-g++}" -std=c++17 -O1 -Wall -Wextra -Werror -I"$root/platform/include" "$root/tests/latency24_fast_test.cpp" -o "$out/t"
"$out/t"
