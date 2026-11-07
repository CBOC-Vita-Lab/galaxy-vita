#!/usr/bin/env bash
# tex11: GXV_TEXDEC_FAST (vgx_texdecode.cpp): the whole-tile decoder byte for byte against the reference (tests/vgx_texdecode_fast.cpp);
# "bench" as the second argument also prints the host decode throughput per format (host timing, not the console's).
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
"${CXX:-g++}" -std=c++17 -O2 -Wall -Wextra -fsanitize=address,undefined -fno-omit-frame-pointer -I"$root/platform/vgx/src" -I"$root/platform/include" \
  "$root/tests/vgx_texdecode_fast.cpp" "$root/platform/vgx/src/vgx_texdecode.cpp" -o "$out/vgx_texdecode_fast"
UBSAN_OPTIONS=halt_on_error=1 "$out/vgx_texdecode_fast" ${2:-}
