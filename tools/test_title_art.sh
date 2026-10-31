#!/usr/bin/env bash
# The player's own title pictures: the host tests of port/title_art.h (logic, deflate, quantizer, PNG, the picture path), the Vita
# worker against a fake console, and the Python checks against zlib, Pillow and the reference script. Optional:
# TITLEART_CAPTURE=<960x544 RGBA title capture> (or local/title-capture.rgba) to include the real-capture comparison (skipped without it).
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
: "${VITASDK:=/opt/vitasdk}"
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
cxx=${CXX:-clang++}
flags=(-std=c++17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer -I"$root/platform/include" -I"$root/tests")
"$cxx" "${flags[@]}" "$root/tests/title_art.cpp" -o "$out/title_art"
"$out/title_art" logic
mkdir -p "$out/syn"
"$out/title_art" synthetic "$out/syn"
# the worker, with the console's headers (types only) and a fake console
"$cxx" "${flags[@]}" -Wno-deprecated-declarations -I"$root/platform/vgx/src" -idirafter "$VITASDK/arm-vita-eabi/include" "$root/tests/title_art_runtime.cpp" -o "$out/runtime"
for mode in disabled enabled-start titleart-off-in-worker first-run write-once package-update old-bubble-version force unsettled not-a-title \
            unsupported-format tear appmeta-denied app-denied both-denied low-heap title-id title-id-invalid title-id-failed corrupt-capture; do
  "$out/runtime" "$mode" | tail -1
done
# 32-bit ARM (the console's width) under qemu, when the cross compiler and qemu-arm are at hand: the made-up title must give the very
# same picture bytes as the host build (no float or integer-width differences)
if command -v arm-linux-gnueabihf-g++ >/dev/null && command -v qemu-arm >/dev/null; then
  arm-linux-gnueabihf-g++ -std=c++17 -O1 -static -I"$root/platform/include" -I"$root/tests" "$root/tests/title_art.cpp" -o "$out/title_art_arm"
  mkdir -p "$out/syn_arm"
  qemu-arm "$out/title_art_arm" logic | tail -1
  qemu-arm "$out/title_art_arm" synthetic "$out/syn_arm" | tail -1
  for n in pic0 bg startup icon0; do cmp "$out/syn/$n.png" "$out/syn_arm/$n.png"; done
  echo "PASS: 32-bit ARM pictures are byte-identical to the host's"
else
  echo "skipped: 32-bit ARM leg (arm-linux-gnueabihf-g++ or qemu-arm not found)"
fi
python3 -m unittest "$root/tests/test_title_art.py" 2>&1 | tail -5
