#!/usr/bin/env bash
# Fur density map byte order (GAL-023): port::fur::BuildMap through the production IA8 decoder, and the GXV_FIX_FURMAP switch
# (default on, read once). No GL, no game data.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
src="$root/platform/vgx/src"
nice "${CXX:-clang++}" -std=c++20 -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=all \
  -I"$root/platform/include" -I"$src" "$root/tests/fur_map.cpp" "$src/vgx_texdecode.cpp" -pthread -o "$out/fur_map"
env -u GXV_FIX_FURMAP "$out/fur_map"
env -u GXV_FIX_FURMAP "$out/fur_map" --env 1
for v in "" 1 0x1 7 invalid 1suffix; do GXV_FIX_FURMAP=$v "$out/fur_map" --env 1; done
for v in 0 0x0 00; do GXV_FIX_FURMAP=$v "$out/fur_map" --env 0; done
echo "fur_map: all passed"
