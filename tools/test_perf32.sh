#!/usr/bin/env bash
# perf32 (20 Nov 2026) host checks of the two exact game-thread switches, both default 0:
#   GXV_PTCL_PRE     tests/perf32_ptcl_pre.cpp (centre test => kernel, 7.5 M quads, a negative control that must fail), then tools/test_jpa_ptcl_cull.sh
#                    (the real JPA units: calls and registers identical with the switch on and in verify mode)
#   GXV_CPULOD_MEMO  tests/perf32_cpulod_memo.cpp (memo against the search, ASan + UBSan, two threads, a negative control that must fail), then tools/test_cpulod.sh
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d "${TMPDIR:-/tmp}/perf32.XXXXXX")}
mkdir -p "$out"
CXX=${CXX:-g++}
"$CXX" -std=c++17 -O2 -Wall -I"$root/platform/include" "$root/tests/perf32_ptcl_pre.cpp" -o "$out/ptcl_pre"
"$out/ptcl_pre"
"$CXX" -std=c++17 -O2 -DPTCL_PRE_SLACK=-0.02f -I"$root/platform/include" "$root/tests/perf32_ptcl_pre.cpp" -o "$out/ptcl_pre_neg"
if "$out/ptcl_pre_neg" > "$out/ptcl_pre_neg.txt"; then echo "negative control (a negative slack) PASSED (it must fail): FAIL"; exit 1; else echo "negative control fails as it must: $(grep -c 'WRONG' "$out/ptcl_pre_neg.txt") lines of wrong answers"; fi
"$CXX" -std=gnu++17 -O1 -g -Wall -fsanitize=address,undefined -fno-sanitize-recover=undefined -pthread -I"$root/platform/include" "$root/tests/perf32_cpulod_memo.cpp" -o "$out/cpulod_memo"
"$out/cpulod_memo"
"$CXX" -std=gnu++17 -O1 -DCPULOD_MEMO_NEG=1 -pthread -I"$root/platform/include" "$root/tests/perf32_cpulod_memo.cpp" -o "$out/cpulod_memo_neg"
if "$out/cpulod_memo_neg" > "$out/cpulod_memo_neg.txt"; then echo "negative control (a memo that trusts its slot) PASSED (it must fail): FAIL"; exit 1; else echo "negative control fails as it must"; fi
bash "$root/tools/test_jpa_ptcl_cull.sh" "$out/jpa"
CXX="$CXX" bash "$root/tools/test_cpulod.sh"
echo "test_perf32: PASS"
