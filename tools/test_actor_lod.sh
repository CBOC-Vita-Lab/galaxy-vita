#!/bin/bash
# GXV_ALOD (actorlod23) host test: port/actor_lod.h's schedule and catch-up against a model actor (tests/actor_lod.cpp).
# PASS, then the three negative controls must FAIL.
set -uo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
out="${TMPDIR:-/tmp}/actor_lod_test.$$"
mkdir -p "$out"
"${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -I"$root/platform/include" ${ALOD_BASE:+-I"$ALOD_BASE/platform/include"} "$root/tests/actor_lod.cpp" -o "$out/actor_lod" || { echo "build FAILED"; exit 2; }
rc=0
"$out/actor_lod" || rc=1
for n in 1 2 3; do
    if ALOD_NEG=$n "$out/actor_lod" >"$out/neg$n.txt" 2>&1; then
        echo "negative control $n did NOT fail: the test cannot see that error"; rc=1
    else
        tail -1 "$out/neg$n.txt"
    fi
done
rm -rf "$out"
[ $rc -eq 0 ] && echo "test_actor_lod: PASS" || echo "test_actor_lod: FAIL"
exit $rc
