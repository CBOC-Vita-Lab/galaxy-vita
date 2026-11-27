#!/usr/bin/env bash
# heap23: GXV_HEAP_BIG's core (platform/include/port/host_big.h) and a 100-stage soak of the console's C heap model, with the negative
# control (tests/heap_soak.cpp says what is checked); heap23b: part 3, the state-list store (GXV_SL_SCENEDROP 0 / 1). No console.
#   tools/test_heap_soak.sh [outdir]      SAN=address,undefined adds sanitizers; HEAP_SOAK_CYCLES / _SEED / _VERBOSE / _MIN_LARGEST_KIB / _SL_CYCLES / _LONG_CYCLES
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
san=()
[[ -n ${SAN:-} ]] && san=(-fsanitize="$SAN" -fno-omit-frame-pointer)
"${CXX:-clang++}" -std=c++17 -O2 -g -Wall -Wextra -I"$root/platform/include" "${san[@]}" "$root/tests/heap_soak.cpp" -o "$out/heap_soak" -pthread
UBSAN_OPTIONS=halt_on_error=1 "$out/heap_soak"
echo "heap_soak: PASS"
