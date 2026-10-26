#!/usr/bin/env bash
# Rear touch aiming on the host: the real vita/runtime/input.cpp against the Vita SDK's headers and stub system calls.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
sdk=${VITASDK:-/opt/vitasdk}
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
# only the SDK's own headers (its C library would clash with the host's)
mkdir -p "$out/inc"
for d in psp2 psp2common vitasdk vitasdk.h; do [ -e "$sdk/arm-vita-eabi/include/$d" ] && ln -s "$sdk/arm-vita-eabi/include/$d" "$out/inc/$d"; done
clang++ -std=c++20 -O1 -g -fsanitize=undefined -fsanitize-trap=undefined -fno-short-enums \
    -isystem "$out/inc" -I"$root/platform/include" -I"$root/vita/runtime" \
    "$root/tests/vita_rear_touch.cpp" "$root/vita/runtime/input.cpp" "$root/platform/src/input/control_bindings.cpp" -o "$out/rear"
"$out/rear"
