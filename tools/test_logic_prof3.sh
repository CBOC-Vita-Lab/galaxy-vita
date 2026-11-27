#!/bin/bash
# game24 (10 Nov): host test of GXV_LOGIC_PROF=3's bookkeeping (platform/src/port/perf.cpp, the real file) under a fake clock
# (tests/logic_prof3.cpp). `bash tools/test_logic_prof3.sh [outdir]`. Prints PASS or FAIL.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d "${TMPDIR:-/tmp}/logic-prof3.XXXXXX")}
mkdir -p "$out"
for log in 1 0; do
    "${CXX:-clang++}" -std=c++20 -O1 -g -DPORT_LOGGING=$log -I"$root/platform/include" -I"$root/decomp/libs/RVL_SDK/include" \
        -c "$root/platform/src/port/perf.cpp" -o "$out/perf$log.o"
    "${CXX:-clang++}" -std=c++20 -O1 -g -I"$root/platform/include" -c "$root/tests/logic_prof3.cpp" -o "$out/test.o"
    "${CXX:-clang++}" "$out/test.o" "$out/perf$log.o" -pthread -no-pie -Wl,--unresolved-symbols=ignore-all -o "$out/logic_prof3_$log"
    echo "== PORT_LOGGING=$log"
    "$out/logic_prof3_$log"
done
