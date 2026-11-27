#!/usr/bin/env bash
# Host-side test of GXV_IND_FLAT's eligibility rule and memo salt (platform/vgx/src/vgx_indflat.h).
#   tools/test_vgx_indflat.sh [output-dir]
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
${CXX:-g++} -std=c++17 -O1 -Wall -Wextra -I"$root/platform/vgx/src" "$root/tests/vgx_indflat_test.cpp" -o "$out/vgx_indflat_test"
"$out/vgx_indflat_test"
# the key of a flattened stage generates a one-read shader (the generator only: tests/vgx_indflat_shader_test.cpp)
${CXX:-g++} -std=gnu++20 -O1 -I"$root/platform/vgx/src" -I"$root/platform/vgx/third_party" -I"$root/platform/include" \
  "$root/tests/vgx_indflat_shader_test.cpp" "$root/platform/vgx/src/vgx_shadergen.cpp" -o "$out/vgx_indflat_shader_test"
"$out/vgx_indflat_shader_test"
