#!/usr/bin/env bash
# fe2-26: the front end's host benchmark, console-weighted, and the exactness test of GXV_FE_STATEFAST.
#
# The stream is tests/vgx_fe_work.cpp's J3D-shaped scene (FEW_PACKETS=250 packets: ~260 material lists, ~540 shape lists, ~370 draws a picture, close to the
# console census of Honeyhive) plus FEW_TOP=75: the top-level state writes of the game's other drawers, in the console census's classes and counts (fedl25
# REPORT2 section 2: 11.4k register writes a picture, 71 % repeats; with the fixture's own 2.8k, ~11.7k state commands a picture, as the console's
# "fe stream detail" line says). It is a SYNTHESISED stream from census numbers, not a capture: no recorded picture of the console exists in the tree.
#
# Part 1 (exactness): for seeds, FEW_TOP 0 / 75 and a set of settings around the switch (GXV_SPLIT 0/2, GXV_BPFAST=0, GXV_REGSHADOW=0, no retained shapes),
#   GXV_FE_STATEFAST=0/1/2 give the same command buffers frame for frame (hash of every picture's draw records, vertex and index bytes), the verify mode
#   (=2) finds no mismatch, the loop takes commands; an AddressSanitizer / UndefinedBehaviorSanitizer build runs the verify mode clean.
# Part 2 (cost): valgrind --tool=callgrind, collecting only inside vgx::ProcessStream (the player's front end; the finishing thread's own work is a
#   separate thread and not counted), FEW_BYPASS=0 (the fixture's rewritten-every-picture lists are 5x the console's 34 a picture), instructions a picture
#   with the switch off and on, and per-command costs by class (FEW_TOPONLY=<class>, FEW_TOPREP=1: a repeat is the same command sent twice).
#   Instruction counts are x86 (the NEON-sized XF matrix loads are scalar here and cost more than on the Vita); ns on this host are not the Vita's.
#   tools/test_vgx_fe2.sh [frames] [seeds]      (CXX: host compiler, default g++; build in $OUT, default /tmp/vgx_fe2; BENCH=0 skips part 2; ASAN=0 skips the sanitizer build)
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
frames=${1:-60}
seeds=${2:-"1 2 3"}
out=${OUT:-/tmp/vgx_fe2}
mkdir -p "$out"
cxx=${CXX:-g++}
srcs=(tests/vgx_fe_work.cpp platform/vgx/src/vgx_state.cpp platform/vgx/src/vgx_draw.cpp platform/vgx/src/vgx_fesplit.cpp
      platform/vgx/src/vgx_dlcache.cpp platform/vgx/src/vgx_guestmem.cpp platform/vgx/src/vgx_bpsync.cpp platform/vgx/src/vgx_shadergen.cpp
      platform/vgx/src/vgx_texcache.cpp platform/vgx/src/vgx_texdecode.cpp platform/vgx/src/vgx_stats.cpp platform/vgx/src/vgx_backend_null.cpp platform/vgx/src/vgx_texcomp.cpp)
cd "$root"
common=(-std=c++17 -g -Iplatform/vgx/src -Iplatform/vgx/third_party -pthread)
nice -n 15 $cxx "${common[@]}" -O2 "${srcs[@]}" -o "$out/few"
if [ "${ASAN:-1}" != 0 ]; then nice -n 15 $cxx "${common[@]}" -O1 -fsanitize=address,undefined -fno-omit-frame-pointer "${srcs[@]}" -o "$out/few-asan"; fi
export ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
# the console's Vita defaults that matter here, as tools/test_vgx_fe_work.sh and the Vita entry set them
console="FEW_CONSOLE=1 FEW_J3DFMT=1 FEW_RETAINED=1 FEW_ODD=0 FEW_PACKETS=250 FEW_BYPASS=0 GXV_RETAINED=1 GXV_DL_VARIANTS=24 GXV_FE_PARALLEL=1 GXV_FECUT_KEYID=1 GXV_FECUT_SLHOT=1 GXV_FECUT_HINT=1 GXV_DIRECT_J3D=1"
fail=0
for seed in $seeds; do
  for top in 0 75; do
    for around in "GXV_SPLIT=1" "GXV_SPLIT=0" "GXV_SPLIT=2" "GXV_BPFAST=0" "GXV_REGSHADOW=0" "GXV_FE_SPLIT=2 GXV_WORKLOAD_PROBE=1" "FEW_RETAINED=0 GXV_RETAINED=0 FEW_J3DFMT=0"; do
      declare -A hash
      for m in 0 1 2; do
        env $console FEW_TOP=$top $around GXV_FE_STATEFAST=$m FEW_LOG=1 "$out/few" "$frames" "$seed" > "$out/run.txt" 2> "$out/run.err"
        hash[$m]=$(grep -E '^frame ' "$out/run.txt" | md5sum | cut -c1-16)
        if [ "$m" = 2 ]; then
          line=$(grep 'fe-work fixture: statefast' "$out/run.err" | head -1)
          bad=$(echo "$line" | sed 's/.*: \([0-9]*\) checked, \([0-9]*\) mismatching.*/\2/'); chk=$(echo "$line" | sed 's/.*: \([0-9]*\) checked, \([0-9]*\) mismatching.*/\1/')
          taken=$(echo "$line" | sed 's/.*; \([0-9]*\) commands taken.*/\1/')
          if [ "${bad:-1}" != 0 ]; then echo "FAIL seed $seed top $top [$around]: verify: $line"; fail=1; fi
          if [ "$top" != 0 ] && [ "${chk:-0}" -lt 100 ] && [ "$around" = "GXV_SPLIT=1" ]; then echo "FAIL seed $seed top $top: only ${chk:-0} repeats checked"; fail=1; fi
          if [ "$around" != "GXV_BPFAST=0" ] && [ "${taken:-0}" -lt 100 ]; then echo "FAIL seed $seed top $top [$around]: only ${taken:-0} commands taken by the loop"; fail=1; fi
        fi
      done
      if [ "${hash[0]}" != "${hash[1]}" ] || [ "${hash[0]}" != "${hash[2]}" ]; then
        echo "FAIL seed $seed top $top [$around]: command buffers differ: off ${hash[0]} on ${hash[1]} verify ${hash[2]}"; fail=1
      fi
    done
  done
done
echo "exactness: STATEFAST 0/1/2 command buffers equal, verify clean (seeds: $seeds, FEW_TOP 0/75, 7 settings each)"
if [ "${ASAN:-1}" != 0 ]; then
  env $console FEW_TOP=75 GXV_FE_STATEFAST=2 GXV_SPLIT=1 "$out/few-asan" 30 1 > /dev/null 2> "$out/asan.err" || { echo "FAIL: sanitizer build"; tail -20 "$out/asan.err"; fail=1; }
  echo "sanitizers (ASan + UBSan, GXV_FE_STATEFAST=2): clean"
fi
# negative control: the verify mode must see a wrong skip (-DGXV_STATEFAST_MUTANT=1: an XF register write counts as a repeat when its first word is the register's)
if [ "${MUTANT:-1}" != 0 ]; then
  nice -n 15 $cxx "${common[@]}" -O2 -DGXV_STATEFAST_MUTANT=1 "${srcs[@]}" -o "$out/few-mutant"
  env $console FEW_TOP=75 GXV_FE_STATEFAST=2 GXV_SPLIT=1 FEW_LOG=1 "$out/few-mutant" "$frames" 1 > "$out/mut.txt" 2> "$out/mut.err" || true
  mbad=$(grep 'fe-work fixture: statefast' "$out/mut.err" | sed 's/.*: \([0-9]*\) checked, \([0-9]*\) mismatching.*/\2/')
  mh=$(grep -E '^frame ' "$out/mut.txt" | md5sum | cut -c1-16)
  if [ "${mbad:-0}" = 0 ]; then echo "FAIL: negative control: verify saw no mismatch in the mutant"; fail=1; else echo "negative control: the mutant's skipped XF writes are seen by the verify mode ($mbad mismatching)"; fi
fi
if [ "${BENCH:-1}" != 0 ]; then
  ir() { env $console "$@" GXV_SPLIT=1 valgrind --tool=callgrind --separate-threads=yes --toggle-collect='vgx::ProcessStream(unsigned char const*, unsigned int, bool)' \
         --callgrind-out-file="$out/ir.out" "$out/few" 60 1 2>&1 | sed -n 's/.*Collected : //p' | head -1; }
  export FEW_PROFILE=1
  b0=$(ir FEW_TOP=0); b1=$(ir FEW_TOP=75); s1=$(ir FEW_TOP=75 GXV_FE_STATEFAST=1); n1=$(ir FEW_TOP=75 GXV_NOOP_FILTER=1)
  unset FEW_PROFILE
  echo "front end instructions a picture (player thread, 60 pictures, host x86):"
  echo "  J3D scene only (FEW_TOP=0)                    $((b0 / 60))"
  echo "  + console-weighted top-level writes           $((b1 / 60))      (top-level writes cost $(( (b1 - b0) / 60 )))"
  echo "  GXV_NOOP_FILTER=1                             $((n1 / 60))      ($(( (b1 - n1) / 60 )) saved)"
  echo "  GXV_FE_STATEFAST=1                            $((s1 / 60))      ($(( (b1 - s1) / 60 )) saved, $(( (b1 - s1) * 1000 / b1 / 10 )).$(( (b1 - s1) * 1000 / b1 % 10 )) % of the front end)"
fi
[ "$fail" = 0 ] && echo PASS || { echo FAILED; exit 1; }
