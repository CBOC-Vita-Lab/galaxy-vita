#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
source tools/host_san.sh
san=($(host_san "${CXX:-c++}" -fsanitize=address,undefined -fno-sanitize-recover=all))
scene=decomp/src/Game/Scene/GameScene.cpp
scenario=decomp/src/Game/System/ScenarioDataParser.cpp
sed -n '/^static bool portTourEnter(.* {$/,/^}/p' "$scene" > "$tmp/tour.inc"
sed -n '/^bool ScenarioData::getValueString(/,/^}/p; /^JMapInfoIter ScenarioData::getScenarioDataIter(/,/^}/p; /^bool ScenarioData::getValueU32(/,/^}/p; /^bool ScenarioData::getValueBool(/,/^}/p' "$scenario" > "$tmp/rows.inc"
sed -n '/^    u32 getCurrentScenarioLayers(/,/^    }/p' "$scenario" > "$tmp/layers.inc"
"${CXX:-c++}" -std=c++17 -DTARGET_PC -include tests/glibc_fabs_shim.h -I"$tmp" -Itests/c183_jmap_shim -Idecomp/include -idirafter platform/include \
    -Idecomp/libs/RVL_SDK/include -Idecomp/libs/JSystem/include ${san[@]+"${san[@]}"} -O1 \
    tests/c183_tour_guard.cpp decomp/src/Game/Util/JMapInfo.cpp -o "$tmp/check"
"$tmp/check"
