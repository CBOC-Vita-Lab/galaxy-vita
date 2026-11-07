#!/usr/bin/env bash
# GXV_LC_NATIVE host proof: os_mem.cpp's answers with the switch unset and set must be byte-identical (tests/lc_native_fast.cpp).
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
"${CXX:-clang++}" -std=c++20 -O1 -w -include "$root/tests/glibc_fabs_shim.h" -I"$root/platform/include" -I"$root/decomp/libs/RVL_SDK/include" \
    -c "$root/platform/src/os/os_mem.cpp" -o "$out/os_mem.o"
"${CXX:-clang++}" -std=c++20 -O1 -no-pie -Wl,-Ttext-segment=0x58000000 "$root/tests/lc_native_fast.cpp" "$out/os_mem.o" -o "$out/lc_native"
export PETARI_NATIVE_ADDRESSES=1
# (no address randomisation: the fixed windows must be free, see the header of tests/lc_native_fast.cpp)
run() { if command -v setarch >/dev/null; then setarch "$(uname -m)" -R "$@"; else "$@"; fi; }
GXV_LC_NATIVE= run "$out/lc_native" > "$out/off.txt"
GXV_LC_NATIVE=1 run "$out/lc_native" > "$out/on.txt"
echo "$(wc -l < "$out/on.txt") pointers; in MEM1: $(awk '$4 == 1' "$out/on.txt" | wc -l), in MEM2: $(awk '$5 == 1' "$out/on.txt" | wc -l)"
if cmp -s "$out/off.txt" "$out/on.txt"; then echo PASS; else echo FAIL; cmp "$out/off.txt" "$out/on.txt" || true; exit 1; fi
