#!/usr/bin/env bash
# dm65: the shadow stencil's operations and the player's own-pixel mark (platform/include/port/shadow_stencil.h) through a model of the GL stencil unit
# (tests/shstencil_test.cpp), and each mutant (dm58's layout, DECR without the counter test), which must FAIL.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=$(mktemp -d "${TMPDIR:-/tmp}/shstencil.XXXXXX")
trap 'rm -rf "$out"' EXIT
cxx=${CXX:-c++}
"$cxx" -std=c++17 -O2 -Wall -Wextra -I"$root/platform/include" "$root/tests/shstencil_test.cpp" -o "$out/t"
"$out/t"
for m in 1 2; do
    "$cxx" -std=c++17 -O2 -DSHSTENCIL_MUTANT=$m -I"$root/platform/include" "$root/tests/shstencil_test.cpp" -o "$out/mut$m"
    if "$out/mut$m" > "$out/mut$m.txt" 2>&1; then echo "shstencil mutant $m: PASSED (it must fail)"; exit 1; fi
    echo "shstencil mutant $m: fails as it must ($(grep -c 'CHECK FAILED' "$out/mut$m.txt") checks)"
done
echo "shstencil: PASS"
