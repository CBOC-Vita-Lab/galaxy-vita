#!/usr/bin/env bash
# GXV_PAR_VIEW host proof: tests/par_exec_view.cpp (the parallel view pass against the serial one, and an executor stress), with the
# production arithmetic extracted from J3DMtxBuffer.cpp. Built with g++ under ThreadSanitizer and under Address/UB sanitizers, in the
# executor's host branch and in its console branch (over the kernel stand-ins of tests/par_stubs).
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
python3 -I - "$root/decomp/src/JSystem/J3DGraphAnimator/J3DMtxBuffer.cpp" "$out/par_core.inc" <<'PY'
import sys
s = open(sys.argv[1]).read()
a = s.index('// PORT_PAR_CORE_BEGIN')
b = s.index('// PORT_PAR_CORE_END', a)
open(sys.argv[2], 'w').write(s[a:b])
PY
CXX=${CXX:-g++}
common=(-std=c++17 -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -ffp-contract=off -I"$root/platform/include" -I"$out")
trials=${1:-300}
for san in thread address,undefined; do
    "$CXX" "${common[@]}" -fsanitize=$san -pthread "$root/tests/par_exec_view.cpp" -o "$out/host-$san"
    echo "== host branch, -fsanitize=$san"
    "$out/host-$san" "$trials"
    "$CXX" "${common[@]}" -fsanitize=$san -pthread -D__vita__ -I"$root/tests/par_stubs" "$root/tests/par_exec_view.cpp" \
        "$root/tests/par_exec_view_stubs.cpp" -o "$out/vita-$san"
    echo "== console branch (kernel stand-ins), -fsanitize=$san"
    "$out/vita-$san" "$trials"
done
"$CXX" "${common[@]}" -pthread -DPAR_TEST_NO_TOUCH "$root/tests/par_exec_view.cpp" -o "$out/negative"
if "$out/negative" 60 >/dev/null; then echo "negative control (no touch) passed: the test cannot see an ordering bug"; exit 1; fi
echo "== negative control (no touch): mismatches found, as it must"
echo "par_exec_view: PASS"
