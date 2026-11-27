#!/usr/bin/env bash
# heap23b: GXV_SL_SCENEDROP (vgx_state.cpp StateListsSceneGone) on the host with the real front end: tests/vgx_sl_scenedrop.cpp, a session
# of 7 galaxies x SL_LOOPS loops, run with GXV_SL_SCENEDROP=0, 1 and 2. Passes when
#   - the register-state digests of every visit are the same in the three modes (a freed entry is parsed again: same state);
#   - mode 0 keeps more entries at the last destroy than at the first (the store grows with the layouts seen), modes 1 / 2 keep only the
#     persistent region's lists after every destroy (1) or none (2);
#   - the bytes held through `new` at the end of a loop do not grow from loop 2 on in modes 1 / 2 (<= 16 KiB a loop and under an eighth of
#     mode 0's: what is left is the geometry cache's tables, swept 600 frames after a list's last use), and do in mode 0 (> 64 KiB a loop).
#   tools/test_vgx_sl_scenedrop.sh     (CXX=g++ by default here; SAN=0 builds without ASan/UBSan; OUT=<dir>)
set -euo pipefail
. "$(cd "$(dirname "$0")" && pwd)/../tests/mp/tmproot.sh"
root=$(cd "$(dirname "$0")/.." && pwd)
out=${OUT:-$MP_TMP/vgx_sl_scenedrop}
mkdir -p "$out"
cd "$root"
srcs=(tests/vgx_sl_scenedrop.cpp platform/vgx/src/vgx_state.cpp platform/vgx/src/vgx_draw.cpp platform/vgx/src/vgx_fesplit.cpp
      platform/vgx/src/vgx_dlcache.cpp platform/vgx/src/vgx_guestmem.cpp platform/vgx/src/vgx_bpsync.cpp platform/vgx/src/vgx_shadergen.cpp
      platform/vgx/src/vgx_texcache.cpp platform/vgx/src/vgx_texdecode.cpp platform/vgx/src/vgx_texcomp.cpp platform/vgx/src/vgx_stats.cpp
      platform/vgx/src/vgx_backend_null.cpp)
san=(-fsanitize=address,undefined)
[[ ${SAN:-1} == 0 ]] && san=()
"${CXX:-g++}" -std=c++17 -O1 -g -DVGX_NO_NEON -Iplatform/vgx/src -Iplatform/vgx/third_party -pthread "${san[@]}" "${srcs[@]}" -o "$out/sl_scenedrop"
fail=0
for mode in 0 1 2; do
  ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 GXV_SL_SCENEDROP=$mode "$out/sl_scenedrop" > "$out/m$mode.txt" 2> "$out/m$mode.err" || { echo "FAIL: mode $mode exited $?"; tail -5 "$out/m$mode.err"; exit 1; }
done
for mode in 1 2; do
  if ! cmp -s "$out/m0.txt" "$out/m$mode.txt"; then
    echo "FAIL: GXV_SL_SCENEDROP=$mode: another register state than mode 0 at $(diff "$out/m0.txt" "$out/m$mode.txt" | head -2 | tail -1)"
    fail=1
  fi
done
entries() { sed -n "s/^visit [0-9.]* ([0-9]* lists): store \([0-9]*\) entries.*/\1/p" "$1"; }
first0=$(entries "$out/m0.err" | head -1); last0=$(entries "$out/m0.err" | tail -1)
max1=$(entries "$out/m1.err" | sort -n | tail -1); max2=$(entries "$out/m2.err" | sort -n | tail -1)
grow() { sed -n 's/^growth-kib-per-loop \(.*\)/\1/p' "$1"; }
g0=$(grow "$out/m0.err"); g1=$(grow "$out/m1.err"); g2=$(grow "$out/m2.err")
echo "mode 0: store entries after the first destroy $first0, after the last $last0; \`new\` growth a loop (loop 2 on) $g0 KiB"
echo "mode 1: store entries after a destroy at most $max1 (the persistent lists: 48); \`new\` growth a loop $g1 KiB"
echo "mode 2: store entries after a destroy at most $max2; \`new\` growth a loop $g2 KiB"
(( last0 > first0 )) || { echo "FAIL: mode 0 did not keep more entries over the session ($first0 -> $last0): the model does not show the growth"; fail=1; }
(( max1 <= 48 )) || { echo "FAIL: mode 1 kept $max1 entries after a destroy (only the 48 persistent lists should stay)"; fail=1; }
(( max2 == 0 )) || { echo "FAIL: mode 2 kept $max2 entries after a destroy"; fail=1; }
# (modes 1 / 2: what is left is the geometry cache's per-list tables, swept 600 frames after a list's last use: noise of a few KiB)
awk -v a="$g1" -v b="$g2" -v c="$g0" 'BEGIN { exit !(a <= 16 && b <= 16 && c > 64 && a * 8 < c && b * 8 < c) }' || { echo "FAIL: \`new\` growth a loop: mode 0 $g0, mode 1 $g1, mode 2 $g2 KiB"; fail=1; }
grep "^mode" "$out/m0.err" "$out/m1.err" "$out/m2.err" | sed 's/^.*\.err://'
[ $fail = 0 ] && echo "PASS: GXV_SL_SCENEDROP 0/1/2 leave the same register state after each of $(grep -c '^visit' "$out/m0.txt") visits' calls; the store is bounded with 1 and 2"
exit $fail
