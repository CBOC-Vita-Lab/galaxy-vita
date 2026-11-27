#!/usr/bin/env bash
# GXV_PIPE25 host test: tests/pipe25.cpp under g++ -fsanitize=thread, -fsanitize=address,undefined, and plain (negative control).
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${OUT:-$(mktemp -d)}
CXX=${CXX:-g++}
f=(-std=c++17 -g -O1 -I"$root/platform/include" "$root/tests/pipe25.cpp" -pthread)
$CXX "${f[@]}" -fsanitize=thread -o "$out/p25-tsan"
$CXX "${f[@]}" -fsanitize=address,undefined -o "$out/p25-asan"
$CXX "${f[@]}" -o "$out/p25-plain"
for s in 1 2; do P25_SLEEP=$s "$out/p25-tsan" 300 40; "$out/p25-asan" 300 40; done
P25_SLEEP=1 P25_NEG=noswap "$out/p25-plain" 300 40
g=(-std=c++17 -g -O1 -I"$root/platform/include" "$root/tests/pipe26.cpp" -pthread)
$CXX "${g[@]}" -fsanitize=thread -o "$out/p26-tsan"
$CXX "${g[@]}" -fsanitize=address,undefined -o "$out/p26-asan"
$CXX "${g[@]}" -o "$out/p26-plain"
for s in 1 2; do P26_SLEEP=$s "$out/p26-tsan" 200 6; "$out/p26-asan" 200 6; done
P26_SLEEP=1 P26_NEG=unfenced "$out/p26-plain" 200 6
# pipe27: the production engine (port/draw_thread.h class Late) over a model of the game's side: in-order publish, late-helper take-over,
# abandon, scene-change join, early join, verify; and the unfenced negative control (the helper reads the live state)
h=(-std=c++17 -g -O1 -I"$root/platform/include" "$root/tests/pipe27.cpp" -pthread)
$CXX "${h[@]}" -fsanitize=thread -o "$out/p27-tsan"
$CXX "${h[@]}" -fsanitize=address,undefined -o "$out/p27-asan"
$CXX "${h[@]}" -o "$out/p27-plain"
P27_SLEEP=1 "$out/p27-tsan" 40 6
"$out/p27-tsan" 40 6
"$out/p27-asan" 40 6
P27_SLEEP=1 P27_NEG=unfenced "$out/p27-plain" 60 6
echo "pipe25 host test: PASS"
