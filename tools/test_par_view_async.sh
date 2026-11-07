#!/usr/bin/env bash
# GXV_PAR_VIEW=2 host proof: tests/par_view_async.cpp (the asynchronous view pass, finished by a worker while the draw and a front-end
# thread read its results, against the serial pass), with the production arithmetic extracted from J3DMtxBuffer.cpp. Built with g++
# under ThreadSanitizer and under Address/UB sanitizers, in the worker's host branch and in its console branch (over the kernel
# stand-ins of tests/par_stubs); then the negative control (no touch at the shape draw) must fail.
#   bash tools/test_par_view_async.sh [trials]
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
common=(-std=c++17 -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -ffp-contract=off -I"$root/platform/include"
        -I"$root/tests" -I"$out")
trials=${1:-200}
export TSAN_OPTIONS=${TSAN_OPTIONS:-halt_on_error=1}
for san in thread address,undefined; do
    "$CXX" "${common[@]}" -fsanitize=$san -pthread "$root/tests/par_view_async.cpp" -o "$out/host-$san"
    echo "== host branch, -fsanitize=$san"
    "$out/host-$san" "$trials"
    "$CXX" "${common[@]}" -fsanitize=$san -pthread -D__vita__ -I"$root/tests/par_stubs" "$root/tests/par_view_async.cpp" \
        "$root/tests/par_exec_view_stubs.cpp" -o "$out/vita-$san"
    echo "== console branch (kernel stand-ins), -fsanitize=$san"
    "$out/vita-$san" "$trials"
done
"$CXX" "${common[@]}" -pthread -DPAR_ASYNC_NO_TOUCH_OUT "$root/tests/par_view_async.cpp" -o "$out/negative"
if "$out/negative" 60 >/dev/null; then echo "negative control (no touch at the shape draw) passed: the test cannot see a missing touch"; exit 1; fi
echo "== negative control (no touch at the shape draw): mismatches found, as it must"
"$CXX" "${common[@]}" -fsanitize=thread -pthread -DPAR_ASYNC_NO_TOUCH_OUT "$root/tests/par_view_async.cpp" -o "$out/negative-tsan"
if "$out/negative-tsan" 20 >/dev/null 2>"$out/neg.err"; then echo "negative control under ThreadSanitizer passed"; exit 1; fi
if grep -q "data race" "$out/neg.err"; then echo "== negative control under ThreadSanitizer: data race reported, as it must"; fi
echo "par_view_async: PASS"
