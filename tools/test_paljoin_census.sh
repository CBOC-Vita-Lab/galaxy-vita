#!/usr/bin/env bash
# paljoin31 (plan-gpu31 A-T1): host test of platform/vgx/src/vgx_paljoin.h (the decision of GXV_PALJOIN_CENSUS) against a synthetic J3D model
# (tests/paljoin_census.cpp), then once per PALJOIN_MUTANT=n: each breaks one rule of the join and the test must fail on it.
#   tools/test_paljoin_census.sh [scratch dir]
set -u
root=$(cd "$(dirname "$0")/.." && pwd)
work=${1:-$(mktemp -d)}
cxx=${CXX:-g++}
mkdir -p "$work"
build() { "$cxx" -std=c++17 -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=undefined "$@" "$root/tests/paljoin_census.cpp" -o "$work/t$MUT" 2> "$work/build$MUT.log"; }
MUT=0
if ! build; then echo "build FAILED"; cat "$work/build0.log" | head -20; exit 2; fi
if ! "$work/t0"; then echo "FAIL: the test fails on the unbroken header"; exit 1; fi
names=("" "state ignored" "registers ignored" "job counter ignored" "model ignored" "cap one too big" "no cap" "every join counted as a reload")
caught=0; missed=0
for MUT in 1 2 3 4 5 6 7; do
  if ! build -DPALJOIN_MUTANT=$MUT; then echo "BROKEN mutant $MUT (does not build)"; missed=$((missed + 1)); continue; fi
  if "$work/t$MUT" > "$work/log$MUT" 2>&1; then echo "MISSED mutant $MUT (${names[$MUT]})"; missed=$((missed + 1)); else echo "CAUGHT mutant $MUT (${names[$MUT]})"; caught=$((caught + 1)); fi
done
echo "paljoin census mutants: $caught caught, $missed missed"
[ $missed -eq 0 ]
