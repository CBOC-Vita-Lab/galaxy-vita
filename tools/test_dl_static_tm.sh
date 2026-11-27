#!/usr/bin/env bash
# GXV_DL_STATIC_TM host test (game24, 10 Nov): the real DlStatic code of ModelManager.cpp (between its cpu60-test markers) with plain texture matrices
# allowed, against stand-ins (tests/dl_static_tm.cpp). The state after every flush must equal the state of full recomputation. Negative controls
# PORT_DLS_NEG=1 (qualifier blind to texture matrices) and PORT_DLS_NEG=2 (plain matrix result secretly reads the view) must FAIL.
#   bash tools/test_dl_static_tm.sh [rounds]
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${OUT:-$(mktemp -d)}
rounds=${1:-200}
sed -n '/cpu60-test-begin/,/cpu60-test-end/p' "$root/decomp/src/Game/LiveActor/ModelManager.cpp" > "$out/dl_static_cut.inc"
CXX=${CXX:-g++}
$CXX -std=c++17 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -I"$out" "$root/tests/dl_static_tm.cpp" -o "$out/dl_static_tm"
ASAN_OPTIONS=detect_leaks=0 "$out/dl_static_tm" "$rounds"
for n in 1 2; do
  if PORT_DLS_NEG=$n ASAN_OPTIONS=detect_leaks=0 "$out/dl_static_tm" "$rounds" > "$out/neg$n.txt" 2>&1; then echo "negative control $n did not fail"; cat "$out/neg$n.txt"; exit 1; fi
  echo "negative control $n fails as it must: $(grep -m1 FAIL "$out/neg$n.txt")"
done
