#!/usr/bin/env bash
# cook24: the "load-split:" disc counters of dvd.cpp (real worker, synthetic files). Same build line as test_dvd_resume.sh, host clang++.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
out=${TMPDIR:-/tmp}/dvd_loadsplit_$$
clang++ -std=c++20 -O1 -g -fms-extensions -DTARGET_PC=1 -DPORT_PLATFORM_CODE=1 -DPORT_RUNTIME_DIAGNOSTICS=0 -DPORT_LOGGING=1 \
  -include string -include iostream -include sstream -include atomic -include mutex -include thread -include condition_variable \
  -include platform/include/port/compat.h -w -Iplatform/include -Idecomp/libs/RVL_SDK/include -ffunction-sections -fdata-sections -Wl,--gc-sections \
  tests/dvd_loadsplit.cpp platform/src/os/os_alarm.cpp -lpthread -o "$out"
"$out"
