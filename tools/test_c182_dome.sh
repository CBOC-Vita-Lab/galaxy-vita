#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
mini=decomp/src/Game/MapObj/MiniatureGalaxy.cpp
pointer=decomp/src/Game/Screen/StarPointerDirector.cpp
sed -n '/^void SphereSelector::sendMsgToAllActor(/,/^}/p' decomp/src/Game/Map/SphereSelector.cpp > "$out/select.inc"
sed -n '/^void SphereSelectorHandle::appear(/,/^}/p' decomp/src/Game/Map/SphereSelectorHandle.cpp >> "$out/select.inc"
sed -n '/^void MiniatureGalaxy::appear(/,/^}/p' "$mini" >> "$out/select.inc"
sed -n '/^void MiniatureGalaxy::init(/,/^}/p' "$mini" | sed -n '/^#ifdef TARGET_PC/,/^#endif/p' > "$out/target-init.inc"
if [[ ! -s "$out/target-init.inc" ]]; then
    sed -n '/^void MiniatureGalaxy::init(/,/^}/p' "$mini" | sed -n '/MR::initStarPointerTarget/p' > "$out/target-init.inc"
fi
sed -n '/^void StarPointerTarget::calcPosition(/,/^}/p' decomp/src/Game/Screen/StarPointerTarget.cpp >> "$out/select.inc"
sed -n '/^    static f32 mtx_identity\[/,/^};  \/\/ namespace/p' "$pointer" | sed '$d' > "$out/holder.inc"
sed -n '/^StarPointerTransformHolder::StarPointerTransformHolder()/,/^}/p' "$pointer" >> "$out/holder.inc"
source tools/host_san.sh
san=($(host_san "${CXX:-clang++}" -fsanitize=address,undefined))
"${CXX:-clang++}" -std=c++17 -DTARGET_PC=1 -O1 -g -ftrivial-auto-var-init=pattern ${san[@]+"${san[@]}"} -I"$out" tests/c182_dome.cpp -o "$out/test"
"$out/test"
