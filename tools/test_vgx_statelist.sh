#!/usr/bin/env bash
# State-list cache (platform/vgx/src/vgx_state.cpp) on the host: tests/vgx_statelist.cpp with GXV_STATELIST=0 (every list parsed),
# 1 and 2 (verify). Passes when the three print the same register-state hash after every call, 1 replays from the cache, and 2
# reports no mismatch.
#   tools/test_vgx_statelist.sh      (host clang; out-of-tree build in $OUT, default /tmp/vgx_statelist)
set -euo pipefail
. "$(cd "$(dirname "$0")" && pwd)/../tests/mp/tmproot.sh"
# (From Producdevity's fork, b7f30ea. CXX=g++ works where clang has no sanitizer runtime.)
root=$(cd "$(dirname "$0")/.." && pwd)
out=${OUT:-$MP_TMP/vgx_statelist}
mkdir -p "$out"
cd "$root"
srcs=(tests/vgx_statelist.cpp platform/vgx/src/vgx_state.cpp platform/vgx/src/vgx_draw.cpp platform/vgx/src/vgx_fesplit.cpp
      platform/vgx/src/vgx_dlcache.cpp platform/vgx/src/vgx_guestmem.cpp platform/vgx/src/vgx_bpsync.cpp platform/vgx/src/vgx_shadergen.cpp
      platform/vgx/src/vgx_texcache.cpp platform/vgx/src/vgx_texdecode.cpp platform/vgx/src/vgx_texcomp.cpp platform/vgx/src/vgx_stats.cpp platform/vgx/src/vgx_backend_null.cpp)
"${CXX:-clang++}" -std=c++17 -O1 -g -DVGX_NO_NEON -Iplatform/vgx/src -Iplatform/vgx/third_party -pthread -fsanitize=address,undefined "${srcs[@]}" -o "$out/statelist"
fail=0
for mode in 0 1 2; do GXV_STATELIST=$mode "$out/statelist" > "$out/s$mode.txt" 2> "$out/s$mode.err"; done
for mode in 1 2; do
  if ! cmp -s "$out/s0.txt" "$out/s$mode.txt"; then
    echo "FAIL: GXV_STATELIST=$mode leaves another register state than the parser at call $(cmp "$out/s0.txt" "$out/s$mode.txt" | awk '{print $NF}')"
    fail=1
  fi
done
grep -q "statelist: [1-9][0-9]* hits" "$out/s1.err" || { echo "FAIL: GXV_STATELIST=1 replayed nothing"; fail=1; }
grep -q " [1-9][0-9]* verified, 0 mismatching" "$out/s2.err" || { echo "FAIL: verify mode: $(cat "$out/s2.err")"; fail=1; }
cat "$out/s1.err"
[ $fail = 0 ] && echo "PASS: state lists leave the parser's register state after each of $(wc -l < "$out/s0.txt") calls (host, not Vita timing)"
exit $fail
