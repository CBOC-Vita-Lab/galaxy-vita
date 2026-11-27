#!/usr/bin/env bash
# GXV_DL_STATIC repeat test (cpu60, 10 Nov): the real code of decomp/src/Game/LiveActor/ModelManager.cpp between its cpu60-test markers on the host
# (tests/dl_static_why.cpp). PORT_DLS_NEG=1 is the negative control (it must FAIL).   bash tools/test_dl_static_why.sh [rounds]
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${OUT:-$(mktemp -d)}
rounds=${1:-200}
sed -n '/cpu60-test-begin/,/cpu60-test-end/p' "$root/decomp/src/Game/LiveActor/ModelManager.cpp" > "$out/dl_static_cut.inc"
CXX=${CXX:-g++}
$CXX -std=c++17 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -I"$out" "$root/tests/dl_static_why.cpp" -o "$out/dl_static_why"
ASAN_OPTIONS=detect_leaks=0 "$out/dl_static_why" "$rounds"
PORT_DLS_XANIM0=1 ASAN_OPTIONS=detect_leaks=0 "$out/dl_static_why" "$rounds"   # (gamecpu25: the switch off)
if PORT_DLS_NEG=1 ASAN_OPTIONS=detect_leaks=0 "$out/dl_static_why" "$rounds" > "$out/neg.txt" 2>&1; then echo "negative control did not fail"; cat "$out/neg.txt"; exit 1; fi
echo "negative control fails as it must: $(grep -m1 FAIL "$out/neg.txt")"
# gamecpu25: negative control for the memo's model identity (a re-pointed Xanime model must not inherit the old model's memo): without it check 1 must FAIL
mkdir -p "$out/noid"
sed 's/? ((static_cast< u64 >(reinterpret_cast< uintptr_t >(model)) \* 0x9E3779B97F4A7C15ull) | 1ull) : 0ull;/? 0ull : 0ull;/' "$out/dl_static_cut.inc" > "$out/noid/dl_static_cut.inc"
if cmp -s "$out/dl_static_cut.inc" "$out/noid/dl_static_cut.inc"; then echo "noid sed did not apply"; exit 1; fi
$CXX -std=c++17 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -I"$out/noid" "$root/tests/dl_static_why.cpp" -o "$out/noid/dl_static_why"
if ASAN_OPTIONS=detect_leaks=0 "$out/noid/dl_static_why" "$rounds" > "$out/noid/out.txt" 2>&1; then echo "identity negative control did not fail"; tail -5 "$out/noid/out.txt"; exit 1; fi
echo "identity negative control fails as it must: $(grep -m1 FAIL "$out/noid/out.txt")"
