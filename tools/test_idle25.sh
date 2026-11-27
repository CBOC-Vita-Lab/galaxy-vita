#!/bin/bash
# idle2525: host test of platform/include/port/idle25.h (tests/idle25.cpp), ASan + UBSan when the compiler has them, else plain.
set -u
root="$(cd "$(dirname "$0")/.." && pwd)"
out="${TMPDIR:-/tmp}/idle25_test.$$"; mkdir -p "$out"
for cxx in "${CXX:-}" g++ clang++; do
    [ -n "$cxx" ] || continue
    command -v "$cxx" >/dev/null 2>&1 || continue
    if "$cxx" -std=gnu++17 -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=undefined -I"$root/platform/include" ${EXTRA:-} "$root/tests/idle25.cpp" -o "$out/idle25" 2>"$out/err"; then
        echo "built with $cxx + ASan/UBSan"; break
    fi
    if "$cxx" -std=gnu++17 -O1 -g -Wall -Wextra -I"$root/platform/include" ${EXTRA:-} "$root/tests/idle25.cpp" -o "$out/idle25" 2>"$out/err"; then
        echo "built with $cxx, PLAIN (no sanitizers)"; break
    fi
done
[ -x "$out/idle25" ] || { echo "build FAILED"; cat "$out/err"; rm -rf "$out"; exit 2; }
"$out/idle25"; rc=$?; rm -rf "$out"; exit $rc
