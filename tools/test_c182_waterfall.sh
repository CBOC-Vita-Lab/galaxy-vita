#!/usr/bin/env bash
set -eu
cd "$(dirname "$0")/.."
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
sed -n '/virtual TVec3f\* getClippingCenterOffset() const {/,/^    }/p' decomp/include/Game/Effect/SimpleEffectObj.hpp > "$out/offset.inc"
source tools/host_san.sh
san=($(host_san "${CXX:-clang++}" -fsanitize=address,undefined -fsanitize-address-use-after-return=always))
"${CXX:-clang++}" -std=c++17 -O1 -g ${san[@]+"${san[@]}"} \
    -Wno-return-stack-address -I"$out" tests/c182_waterfall.cpp -o "$out/test"
"$out/test"
