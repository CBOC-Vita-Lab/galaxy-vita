#!/usr/bin/env bash
# Color8's u32 view is 0xRRGGBBAA on a little-endian host (GXV_FIX_COLOR8, default 1): the effect list's red ground-pound and black-hole
# trail colours, a million random values, the call sites that build a Color8 from a u32 literal, and the old layout with the switch at 0.
# No GL, no game data.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
nice "${CXX:-clang++}" -std=gnu++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all \
  -DTARGET_PC=1 -DVERSION=0 -fshort-wchar -fsigned-char -fno-strict-aliasing -fwrapv -fms-extensions \
  -Wno-register -Wno-multichar -Wno-writable-strings \
  -I"$root/platform/include" -isystem "$root/decomp/include" -isystem "$root/decomp/libs/JSystem/include" -isystem "$root/decomp/libs/RVL_SDK/include" \
  -include "$root/platform/include/port/compat.h" "$root/tests/color8_byte_order.cpp" -o "$out/color8_byte_order"
env -u GXV_FIX_COLOR8 "$out/color8_byte_order" 1
for v in "" 1 0x1 2 -1; do GXV_FIX_COLOR8=$v "$out/color8_byte_order" 1; done
for v in 0 0x0 00 invalid; do GXV_FIX_COLOR8=$v "$out/color8_byte_order" 0; done
echo "color8: all passed"
