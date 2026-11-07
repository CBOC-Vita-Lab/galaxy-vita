#!/usr/bin/env bash
# Host test of the key animation count fix (GAL-018): the cooker's unsigned counts and the load-time repair of
# data converted by the older cooker. Runs the Python cooker test, then tests/anm_key_repair.cpp under ASan/UBSan.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
cd "$root"
python3 -m unittest discover -s tests -p test_cook_ank1.py
python3 -I tests/anm_key_fixture.py "$out/fixtures"
nice "${CXX:-clang++}" -std=c++20 -O1 -g -fno-strict-aliasing -fsanitize=address,undefined -fno-sanitize-recover=all \
    -Iplatform/include tests/anm_key_repair.cpp -pthread -o "$out/anm_key_repair"
"$out/anm_key_repair" "$out/fixtures"
# GXV_FIX_BCKCNT is read once: unset, empty, non-zero and unreadable values leave the repair on; 0 turns it off
env -u GXV_FIX_BCKCNT "$out/anm_key_repair" --env 1
for v in "" 1 0x1 7 invalid 1suffix; do GXV_FIX_BCKCNT=$v "$out/anm_key_repair" --env 1; done
for v in 0 0x0 00; do GXV_FIX_BCKCNT=$v "$out/anm_key_repair" --env 0; done
echo "anm_key_repair: all passed"
