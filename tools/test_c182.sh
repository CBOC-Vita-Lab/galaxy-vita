#!/usr/bin/env bash
# Offline regression gate; no console, save data or network access needed.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
if ! command -v llvm-profdata >/dev/null && command -v xcrun >/dev/null; then
    mkdir "$tmp/tools"
    ln -s "$(xcrun --find llvm-profdata)" "$tmp/tools/llvm-profdata"
    PATH="$tmp/tools:$PATH"
fi
export OUT="$tmp/bin" TMPDIR="$tmp"
for suite in hrw26 hrw27 hrw28 hrw36 hrw_steady mario_time hrw_settle hrw41 speed_guard mode_keys mpgate c182_cpu c182_quad_decode c182_dome dome_zoom c182_firstvisit fur25 c182_waterfall; do
    printf '\n== %s ==\n' "$suite"
    bash "tools/test_${suite}.sh"
done
"${CXX:-c++}" -std=c++17 -Iplatform/vgx/include -Iplatform/vgx/src tests/c182_pair_depth.cpp -o "$tmp/pair-depth"
"$tmp/pair-depth"
python3 -m unittest discover -s tests -p test_pgo_build.py -v
python3 tools/test_c182_seed.py
