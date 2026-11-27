#!/usr/bin/env bash
# sampler30: host test of the sampling profiler's ring, settings and report writer (tests/sampler_ring_test.cpp) under ASan + UBSan, then the report is
# run through tools/sampler/symbolize.py with a fake `nm` listing (no Vita SDK needed).
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
cxx=${CXX:-g++}
nice -n 10 "$cxx" -std=gnu++20 -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-omit-frame-pointer -I"$root/platform/include" \
  "$root/tests/sampler_ring_test.cpp" -o "$out/sampler_ring_test"
UBSAN_OPTIONS=halt_on_error=1 "$out/sampler_ring_test" "$out/report.txt"
# the fake ELF: the anchor sits 0x10000 higher at run time than in the ELF, so every address is 0x10000 above its ELF address
cat > "$out/nm.txt" <<'NM'
81000000 00000100 T port_sampler_boot
80ff0000 00001000 T Alpha(int)
80ff2000 00001000 T Beta()
80ff3000 00001000 t Gamma
80ff4000 00001000 T Delta(char const*)
80ff5000 00001000 T RenderThing()
NM
sed -i 's/anchor port_sampler_boot 0x81234560/anchor port_sampler_boot 0x81010000/' "$out/report.txt"
python3 -I "$root/tools/sampler/symbolize.py" "$out/report.txt" --nm-text "$out/nm.txt" --top 5 > "$out/symbolized.txt"
cat "$out/symbolized.txt"
grep -q "Alpha(int)" "$out/symbolized.txt"
grep -q "Alpha(int)  <-  Beta()" "$out/symbolized.txt"
grep -q "thread game .*waiting 8" "$out/symbolized.txt"
grep -q "65 (delay)" "$out/symbolized.txt"
grep -q "RenderThing()" "$out/symbolized.txt"
grep -q "load offset" "$out/symbolized.txt"
echo "test_sampler: ok"
