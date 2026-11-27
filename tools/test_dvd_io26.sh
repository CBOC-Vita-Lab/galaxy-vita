#!/usr/bin/env bash
# disc26: the drive worker's play-time policies (long reads in pieces with an urgent request between them, per-file read-ahead windows,
# the learned prefetch cache, the "io:" census) with the real worker and synthetic files. Same build line as test_dvd_loadsplit.sh.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
out=${TMPDIR:-/tmp}/dvd_io26_$$
clang++ -std=c++20 -O1 -g -fms-extensions -DTARGET_PC=1 -DPORT_PLATFORM_CODE=1 -DPORT_RUNTIME_DIAGNOSTICS=1 -DPORT_LOGGING=1 \
  -include string -include iostream -include sstream -include atomic -include mutex -include thread -include condition_variable -include functional \
  -include platform/include/port/compat.h -w -Iplatform/include -Idecomp/libs/RVL_SDK/include -ffunction-sections -fdata-sections -Wl,--gc-sections \
  tests/dvd_io26.cpp platform/src/os/os_alarm.cpp -lpthread -o "$out"
"$out"
rm -f "$out"
