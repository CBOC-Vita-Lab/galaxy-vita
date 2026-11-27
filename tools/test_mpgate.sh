#!/bin/bash
# MP3 lodgate: host test of platform/include/port/mp_gate.h (tests/mpgate.cpp), ASan + UBSan when the compiler has them, else plain.
set -u
root="$(cd "$(dirname "$0")/.." && pwd)"
out="${TMPDIR:-/tmp}/mpgate_test.$$"; mkdir -p "$out"
for cxx in "${CXX:-}" g++ clang++; do
    [ -n "$cxx" ] || continue
    command -v "$cxx" >/dev/null 2>&1 || continue
    if "$cxx" -std=gnu++17 -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=undefined -I"$root/platform/include" "$root/tests/mpgate.cpp" -o "$out/mpgate" 2>"$out/err"; then
        echo "built with $cxx + ASan/UBSan"; break
    fi
    if "$cxx" -std=gnu++17 -O1 -g -Wall -Wextra -I"$root/platform/include" "$root/tests/mpgate.cpp" -o "$out/mpgate" 2>"$out/err"; then
        echo "built with $cxx, PLAIN (no sanitizers)"; break
    fi
done
[ -x "$out/mpgate" ] || { echo "build FAILED"; cat "$out/err"; rm -rf "$out"; exit 2; }
"$out/mpgate"; rc=$?; rm -rf "$out"; exit $rc
