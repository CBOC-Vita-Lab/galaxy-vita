#!/usr/bin/env bash
# prog32b: no CPU / draw cut may take an actor a star can depend on (tests/objective_cuts.cpp). Header + decomp source scan only: no game build.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
nice -n 10 "${CXX:-g++}" -std=c++17 -O1 -g -Wall -Wextra -Werror -Wno-parentheses -DDECOMP_ROOT="\"$root/decomp\"" -I"$root/platform/include" "$root/tests/objective_cuts.cpp" -o "$out/objective_cuts"
"$out/objective_cuts"
