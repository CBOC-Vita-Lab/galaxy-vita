#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
# glibc_fabs_shim.h: glibc's <math.h> declares __fabs/__fabsf, which port/ppc_intrinsics.h defines (harmless on other hosts).
args=(-std=c++17 -DTARGET_PC -include tests/glibc_fabs_shim.h -Itests/c183_jmap_shim -Idecomp/include -idirafter platform/include -Idecomp/libs/RVL_SDK/include -Idecomp/libs/JSystem/include
      tests/c183_jmap.cpp decomp/src/Game/Util/JMapInfo.cpp)
source tools/host_san.sh
san=($(host_san "${CXX:-c++}" -fsanitize=address,undefined))
"${CXX:-c++}" ${san[@]+"${san[@]}"} -O1 "${args[@]}" -o "$tmp/asan"
"$tmp/asan" "$@"
"${CXX:-c++}" -O2 "${args[@]}" -o "$tmp/bench"
"$tmp/bench" "$@"
