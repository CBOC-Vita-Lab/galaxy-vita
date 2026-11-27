#!/bin/bash
# par25: host test of platform/include/port/par25.h (tests/par25.cpp): the Judge, the Honeyhive windows, the report line; then the Gate under two real threads
# with ThreadSanitizer (g++'s libtsan: clang's runtime is not installed here); then the negative control (PAR25_NEG=1), which must fail.
set -u
root="$(cd "$(dirname "$0")/.." && pwd)"
out="$(mktemp -d "${TMPDIR:-/tmp}/par25.XXXXXX")"
trap 'rm -rf "$out"' EXIT
CXX="${CXX:-g++}"
"$CXX" -std=gnu++17 -O1 -g -Wall -Wextra -fsanitize=thread -pthread -I"$root/platform/include" "$root/tests/par25.cpp" -o "$out/par25_tsan" || { echo "build FAILED"; exit 2; }
"$CXX" -std=gnu++17 -O1 -g -Wall -fsanitize=address,undefined -fno-sanitize-recover=undefined -pthread -I"$root/platform/include" "$root/tests/par25.cpp" -o "$out/par25_asan" || { echo "build FAILED"; exit 2; }
echo "== ThreadSanitizer"; "$out/par25_tsan" || exit 1
echo "== ASan + UBSan"; "$out/par25_asan" > /dev/null || exit 1; echo ok
echo "== negative control (must fail)"
if PAR25_NEG=1 "$out/par25_asan" > "$out/neg.txt" 2>&1; then echo "NEGATIVE CONTROL DID NOT FAIL"; cat "$out/neg.txt"; exit 1; else grep -c FAIL "$out/neg.txt" | sed 's/^/failed checks: /'; echo "ok (it failed, as it must)"; fi
