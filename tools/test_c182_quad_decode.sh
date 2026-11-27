#!/usr/bin/env bash
# Default: use the real planner and decoder under ASan/UBSan. "bench": optimized host CPU timings.
set -euo pipefail
cd "$(dirname "$0")/.."
d=$(mktemp -d)
trap 'rm -rf "$d"' EXIT
link=(-Wl,--gc-sections)
if [[ $(uname -s) == Darwin ]]; then link=(-Wl,-dead_strip); fi
flags=(-std=c++17 -ffunction-sections -fdata-sections -DVGX_NO_NEON -DPORT_LOGGING=0
       -DPORT_RUNTIME_DIAGNOSTICS=0 -Iplatform/include -Iplatform/vgx/src -Iplatform/vgx/third_party)
if [[ ${1:-} == bench ]]; then
    flags+=(-O2)
else
    source tools/host_san.sh
    san=($(host_san "${CXX:-clang++}" -fsanitize=address,undefined -fno-omit-frame-pointer))
    flags+=(-O1 -g ${san[@]+"${san[@]}"})
fi
"${CXX:-clang++}" "${flags[@]}" tests/c182_quad_decode.cpp "${link[@]}" -o "$d/check"
if [[ ${1:-} == bench ]]; then "$d/check" bench; exit; fi
export UBSAN_OPTIONS=halt_on_error=1
"$d/check"
GXV_QUAD_DECODE=0 "$d/check"
GXV_POSDEFAULT=2 "$d/check"
GXV_DECODE=0 "$d/check"
