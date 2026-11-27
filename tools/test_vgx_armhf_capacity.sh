#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=$(mktemp -d "${TMPDIR:-/tmp}/vgx-armhf-capacity.XXXXXX")
trap 'rm -rf "$out"' EXIT
"${CXX:-clang++}" -std=c++17 -O1 -g -fsanitize=address,undefined \
  "$root/tests/vgx_armhf_capacity.cpp" "$root/armhf/glue/vgl_stub.cpp" -o "$out/query"
"$out/query"
printf '%s\n' 'PASS ARMHF no-GPU external capacity stand-in; not native occupancy or performance evidence'
