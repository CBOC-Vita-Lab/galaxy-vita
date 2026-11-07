#!/bin/bash
# shadowmemo (6 Nov): host proof of port/shadow_memo.h, the rule behind GXV_SHADOWMEMO (tests/shadowmemo_test.cpp). Prints PASS or FAIL.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d "${TMPDIR:-/tmp}/shadowmemo.XXXXXX")}
mkdir -p "$out"
"${CXX:-g++}" -std=gnu++17 -O2 -Wall -Wextra -Wno-missing-field-initializers -I"$root/platform/include" "$root/tests/shadowmemo_test.cpp" -o "$out/shadowmemo_test"
"$out/shadowmemo_test"
