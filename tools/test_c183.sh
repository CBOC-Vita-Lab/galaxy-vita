#!/usr/bin/env bash
# Optional argument: a local game/ directory for checks against the owner's cooked data.
set -euo pipefail
cd "$(dirname "$0")/.."
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
source tools/host_san.sh
san=($(host_san "${CXX:-c++}" -fsanitize=address,undefined))
"${CXX:-c++}" -std=c++17 ${san[@]+"${san[@]}"} -Iplatform/include tests/title_cloud.cpp -o "$tmp/title-cloud"
"$tmp/title-cloud"
if (( $# )); then
    game=$(cd "$1" && pwd)
    bash tools/test_c183_jmap.sh "$game/sys/GalaxyID.bcsv"
    bash tools/test_c183_rail.sh "$game/files/StageData"
else
    bash tools/test_c183_jmap.sh
    bash tools/test_c183_rail.sh
fi
bash tools/test_vgx_la88_mips.sh
bash tools/test_c183_depth_mark.sh
bash tools/test_c183_tour.sh
bash tools/test_popin.sh
bash tools/test_c182.sh
