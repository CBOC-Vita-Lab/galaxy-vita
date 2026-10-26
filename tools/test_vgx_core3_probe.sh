#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d "${TMPDIR:-/tmp}/vgx-core3.XXXXXX")}
mkdir -p "$out"
"${CXX:-clang++}" -std=c++20 -O2 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I"$root/platform/vgx/vita" "$root/tests/vgx_core3_probe.cpp" -o "$out/core3"
export ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1
"$out/core3"
"${CXX:-clang++}" -std=c++20 -O2 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
    -DVGX_NO_NEON -DPORT_RUNTIME_DIAGNOSTICS=0 -I"$root/platform/vgx/vita" -I"$root/platform/vgx/src" \
    "$root/tests/vgx_core3_probe_vita.cpp" -o "$out/core3-vita"
for scenario in off on invalid create-fail start-fail pin-fail info-fail wrong-mask spin-clock; do
    "$out/core3-vita" "$scenario"
done
