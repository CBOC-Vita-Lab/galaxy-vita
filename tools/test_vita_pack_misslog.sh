#!/usr/bin/env bash
# packrefill25: host test of the shader pack's miss log (ASan + UBSan) and of its Python reader tools/pack25/misskeys.py over the lines the test writes.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${TMPDIR:-/tmp}/vita_pack_misslog_$$
mkdir -p "$out"
g++ -std=c++17 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined -Wall -Wextra -pthread \
  "$root/tests/vita_pack_misslog.cpp" -o "$out/test"
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 "$out/test" "$out"
py="python3 -I $root/tools/pack25/misskeys.py"
$py keys "$out/misses.log" --out "$out/names.txt" | tee "$out/keys.txt"
grep -q "^40 distinct keys" "$out/keys.txt"
test "$(wc -l < "$out/names.txt")" = 40
# the summary line says compiled-in-play 0, there is a banner, but a window of 60 s is there: check passes only with the 40 misses allowed
if $py check "$out/misses.log" > "$out/check0.txt"; then echo "check without --allow must fail"; exit 1; fi
grep -q "40 distinct pack misses" "$out/check0.txt"
$py check "$out/misses.log" --allow "$out/names.txt"
# an in-play compile line and a failure line make it fail even when every miss is allowed
cp "$out/misses.log" "$out/bad.log"
echo "[  70.000] pack: a shader compile ran in play: stage=099e9a42 412 ms (n=1 in play)" >> "$out/bad.log"
if $py check "$out/bad.log" --allow "$out/names.txt" > "$out/check1.txt"; then echo "in-play compile must fail"; exit 1; fi
grep -q "shader compile in play" "$out/check1.txt"
# a log without the banner proves nothing
grep -v "GXV_PACK_MISSLOG=1" "$out/misses.log" > "$out/nobanner.log"
if $py check "$out/nobanner.log" --allow "$out/names.txt" > "$out/check2.txt"; then echo "no banner must fail"; exit 1; fi
grep -q "no 'GXV_PACK_MISSLOG=1' banner" "$out/check2.txt"
echo "test_vita_pack_misslog: PASS"
