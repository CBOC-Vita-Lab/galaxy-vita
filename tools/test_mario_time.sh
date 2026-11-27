#!/usr/bin/env bash
# mariolag31: host test of port/mario_time.h (GXV_HRW_MARIOTIME: the jump arc, the auto rule, the census), then the same test against the header's mutants
# (MARIOTIME_MUTANT 1..7): each must FAIL.
set -uo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${OUT:-/tmp/mario_time_test}; mkdir -p "$out"
CXX=${CXX:-g++}
build() {  # $1 = binary, $2 = mutant number
    nice -n 10 $CXX -std=c++17 -O1 -Wall -Wextra -DMARIOTIME_MUTANT="$2" -I"$root/platform/include" "$root/tests/mario_time_test.cpp" -o "$1"
}
build "$out/base" 0 || exit 1
"$out/base" || { echo "BASE FAILED"; exit 1; }
killed=0; total=0
for m in 1 2 3 4 5 6 7; do
    ((total++))
    if build "$out/mut$m" "$m" 2>/dev/null && "$out/mut$m" >/dev/null 2>&1; then echo "MUTANT SURVIVED: $m"; else ((killed++)); fi
done
echo "mutants killed: $killed of $total"
[ "$killed" -eq "$total" ]
