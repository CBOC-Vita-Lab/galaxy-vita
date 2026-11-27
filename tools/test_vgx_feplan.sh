#!/usr/bin/env bash
# Mixed display lists' plans (GXV_FEPLAN, platform/vgx/src/vgx_state.cpp "mixed display lists: exact plans") and their census
# (GXV_FEPLAN_CENSUS) on the host. There are no recorded GX captures of the game in the repository, so the two fixtures are:
#   1. tests/vgx_feplan.cpp: random mixed lists (state + draws + CALL_DLs inside + NOPs + masks + indexed loads + XF rows, vertex-format
#      writes before draws, unknown opcodes, draws past the end, cut commands), called through CallList with the list's vertex format
#      or another one, rewritten / changed / written next to; a hash of the State after every call and of every command buffer.
#   2. tests/vgx_fe_work.cpp FEW_MIXED=12: the J3D-shaped scene with game-shaped mixed lists (TEV colours, material colour, an indexed
#      matrix load and a strip per segment; rewritten every picture, animated, another vertex format every 7th picture).
# Passes when, in every setting of the switches around them, GXV_FEPLAN=1 and =2 print exactly what GXV_FEPLAN=0 prints (state and
# command buffers), =1 replays plans (and meets each refusal and fallback case), =2 checks plans with 0 mismatching; the census prints its
# lines; and the negative controls fail as they must: -DGXV_FEPLAN_MUTANT=1 (a state operation left out after a draw) and =2 (the
# vertex size not checked) make GXV_FEPLAN=1 differ from =0 and GXV_FEPLAN=2 report mismatches. Built with AddressSanitizer +
# UndefinedBehaviorSanitizer (fixture 1) .
#   tools/test_vgx_feplan.sh [calls] [seeds] [fe-work frames]     (CXX: default g++; build in $OUT, default a directory of tests/mp/tmproot.sh)
set -euo pipefail
. "$(cd "$(dirname "$0")" && pwd)/../tests/mp/tmproot.sh"
root=$(cd "$(dirname "$0")/.." && pwd)
calls=${1:-4000}
seeds=${2:-"1 2 3"}
frames=${3:-60}
out=${OUT:-$MP_TMP/vgx_feplan}
mkdir -p "$out"
cxx=${CXX:-g++}
cd "$root"
lib=(platform/vgx/src/vgx_draw.cpp platform/vgx/src/vgx_fesplit.cpp platform/vgx/src/vgx_dlcache.cpp platform/vgx/src/vgx_guestmem.cpp
     platform/vgx/src/vgx_bpsync.cpp platform/vgx/src/vgx_shadergen.cpp platform/vgx/src/vgx_texcache.cpp platform/vgx/src/vgx_texdecode.cpp
     platform/vgx/src/vgx_texcomp.cpp platform/vgx/src/vgx_stats.cpp platform/vgx/src/vgx_backend_null.cpp)
common=(-std=c++17 -g -DVGX_NO_NEON -Iplatform/vgx/src -Iplatform/include -Iplatform/vgx/third_party -pthread)
san=(-O1 -fsanitize=address,undefined -fno-omit-frame-pointer)
objs() { # objs <dir> <flags...>: the front end's other units, once per flavour
  local d=$1; shift; mkdir -p "$d"
  for f in "${lib[@]}"; do local o="$d/$(basename "$f" .cpp).o"; [ -s "$o" ] || $cxx "${common[@]}" "$@" -c "$f" -o "$o"; done
}
objs "$out/san" "${san[@]}"
objs "$out/o2" -O2
for v in "" 1 2; do # the state unit: plain, and the two negative controls
  def=(); [ -n "$v" ] && def=(-DGXV_FEPLAN_MUTANT=$v)
  $cxx "${common[@]}" "${san[@]}" "${def[@]}" -c platform/vgx/src/vgx_state.cpp -o "$out/san/state$v.o"
  $cxx "${common[@]}" "${san[@]}" -c tests/vgx_feplan.cpp -o "$out/san/test.o"
  $cxx "${common[@]}" "${san[@]}" "$out/san/test.o" "$out/san/state$v.o" "$out"/san/vgx_*.o -o "$out/feplan$v"
done
$cxx "${common[@]}" -O2 -c platform/vgx/src/vgx_state.cpp -o "$out/o2/state.o"
$cxx "${common[@]}" -O2 -c tests/vgx_fe_work.cpp -o "$out/o2/few.o"
$cxx "${common[@]}" -O2 "$out/o2/few.o" "$out/o2/state.o" "$out"/o2/vgx_*.o -o "$out/few"
export ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
# (the State holds host pointers into the fixture's memory: runs are compared with address randomisation off)
norand=(); if setarch "$(uname -m)" -R true 2>/dev/null; then norand=(setarch "$(uname -m)" -R); fi
fail=0
num() { sed -n "s/.*$1.*/\\1/p" "$2" | head -1; }
# ---- 1. random mixed lists
for seed in $seeds; do
  for around in "" "GXV_SLDELTA=0" "GXV_BPFAST=0" "GXV_REGSHADOW=0" "GXV_FE_STATEFAST=1" "GXV_FE_STATEFAST=2" "GXV_SLBYTES=0" "GXV_NOOP_FILTER=1" \
                "GXV_SLFAST=0" "GXV_DL_TINY=15" "GXV_STATELIST=2" "GXV_FECUT_SLHOT=1" "GXV_FEPLAN_CENSUS=1"; do
    env $around GXV_FEPLAN=0 "${norand[@]}" "$out/feplan" "$calls" "$seed" > "$out/p0.txt" 2> "$out/p0.err" || { echo "FAIL seed $seed [$around] FEPLAN=0 crashed"; tail -5 "$out/p0.err"; fail=1; continue; }
    for m in 1 2; do
      env $around GXV_FEPLAN=$m "${norand[@]}" "$out/feplan" "$calls" "$seed" > "$out/p$m.txt" 2> "$out/p$m.err" || { echo "FAIL seed $seed [$around] FEPLAN=$m crashed"; tail -5 "$out/p$m.err"; fail=1; continue; }
      if ! cmp -s "$out/p0.txt" "$out/p$m.txt"; then
        echo "FAIL seed $seed [$around]: GXV_FEPLAN=$m differs from the parser: $(diff "$out/p0.txt" "$out/p$m.txt" | head -2 | tr '\n' ' ')"; fail=1
      fi
    done
    r1=$(num ': \([0-9]*\) replays' "$out/p1.err"); mk=$(num 'replays, \([0-9]*\) plans made' "$out/p1.err")
    rf=$(num 'made, \([0-9]*\) refused' "$out/p1.err"); fb=$(num '(vertex format), \([0-9]*\) vertex size' "$out/p1.err")
    ck=$(num 'verify: \([0-9]*\) plans checked' "$out/p2.err"); bad=$(num 'checked, \([0-9]*\) mismatching' "$out/p2.err")
    if [ "${r1:-0}" -lt 500 ] || [ "${mk:-0}" -lt 20 ] || [ "${rf:-0}" -lt 1 ] || [ "${fb:-0}" -lt 1 ] || [ "${ck:-0}" -lt 500 ] || [ "${bad:-1}" != 0 ]; then
      echo "FAIL seed $seed [$around]: replays ${r1:-?}, made ${mk:-?}, refused ${rf:-?}, size changed ${fb:-?}; verify checked ${ck:-?}, mismatching ${bad:-?}"; fail=1
    fi
    [ -z "$around" ] && echo "     seed $seed: $(tail -1 "$out/p1.err" | cut -c1-200)"
  done
  # census lines (log only: the output must not change)
  GXV_FEPLAN_CENSUS=20 FEPLAN_LOG=1 GXV_FEPLAN=0 "${norand[@]}" "$out/feplan" "$calls" "$seed" > "$out/c0.txt" 2> "$out/c0.err"
  GXV_FEPLAN_CENSUS=20 FEPLAN_LOG=1 GXV_FEPLAN=1 "${norand[@]}" "$out/feplan" "$calls" "$seed" > "$out/c1.txt" 2> "$out/c1.err"
  if ! grep -q "feplan-census: .* a plan could take [1-9]" "$out/c0.err" || ! grep -q "feplan-census: costliest lists" "$out/c0.err" || \
     ! grep -q "plan replays [1-9]" "$out/c1.err" || ! cmp -s "$out/c0.txt" "$out/p0.txt"; then
    echo "FAIL seed $seed: census lines missing or the census changed the output"; fail=1
  fi
  # negative controls
  for v in 1 2; do
    GXV_FEPLAN=0 "${norand[@]}" "$out/feplan$v" "$calls" "$seed" > "$out/m0.txt" 2> /dev/null || true # (its own reference: the State holds host addresses, which another binary places elsewhere)
    GXV_FEPLAN=1 "${norand[@]}" "$out/feplan$v" "$calls" "$seed" > "$out/m1.txt" 2> /dev/null || true
    GXV_FEPLAN=2 "${norand[@]}" "$out/feplan$v" "$calls" "$seed" > "$out/m2.txt" 2> "$out/m2.err" || true
    mbad=$(num 'checked, \([0-9]*\) mismatching' "$out/m2.err")
    if cmp -s "$out/m0.txt" "$out/m1.txt"; then echo "FAIL seed $seed: mutant $v: GXV_FEPLAN=1 did not change the output (the test cannot see it)"; fail=1; fi
    if [ "${mbad:-0}" -lt 1 ]; then echo "FAIL seed $seed: mutant $v: the verify mode saw no mismatch"; fail=1; fi
    if ! cmp -s "$out/m0.txt" "$out/m2.txt"; then echo "FAIL seed $seed: mutant $v: GXV_FEPLAN=2 did not keep the parser's output"; fail=1; fi
    echo "     seed $seed: mutant $v caught (verify mismatching ${mbad:-?})"
  done
done
# ---- 2. the J3D-shaped scene with mixed lists
for seed in $seeds; do
  for around in "" "GXV_SPLIT=1" "GXV_SLDELTA=0" "GXV_FE_STATEFAST=1" "FEW_CONSOLE=1" "GXV_RETAINED=1 FEW_RETAINED=1" "GXV_FEPLAN_CENSUS=1"; do
    env $around FEW_MIXED=12 GXV_FEPLAN=0 "${norand[@]}" "$out/few" "$frames" "$seed" > "$out/w0.txt" 2> "$out/w0.err"
    for m in 1 2; do
      env $around FEW_MIXED=12 GXV_FEPLAN=$m "${norand[@]}" "$out/few" "$frames" "$seed" > "$out/w$m.txt" 2> "$out/w$m.err"
      if ! cmp -s "$out/w0.txt" "$out/w$m.txt"; then echo "FAIL fe-work seed $seed [$around]: GXV_FEPLAN=$m differs"; diff "$out/w0.txt" "$out/w$m.txt" | head -2; fail=1; fi
    done
    r1=$(num 'feplan: \([0-9]*\) replays' "$out/w1.err"); ck=$(num 'verify: \([0-9]*\) plans checked' "$out/w2.err"); bad=$(num 'checked, \([0-9]*\) differing' "$out/w2.err")
    if [ "${r1:-0}" -lt 100 ] || [ "${ck:-0}" -lt 100 ] || [ "${bad:-1}" != 0 ]; then echo "FAIL fe-work seed $seed [$around]: replays ${r1:-?}, checked ${ck:-?}, differing ${bad:-?}"; fail=1; fi
    [ -z "$around" ] && echo "     fe-work seed $seed: $(grep 'feplan:' "$out/w1.err" | cut -c1-200)"
  done
  # the scene without mixed lists is the scene as before (the switch on changes nothing there)
  GXV_FEPLAN=0 "${norand[@]}" "$out/few" "$frames" "$seed" > "$out/n0.txt" 2> /dev/null
  GXV_FEPLAN=1 "${norand[@]}" "$out/few" "$frames" "$seed" > "$out/n1.txt" 2> /dev/null
  cmp -s "$out/n0.txt" "$out/n1.txt" || { echo "FAIL fe-work seed $seed: FEPLAN=1 changed the scene without mixed lists"; fail=1; }
done
FEW_LOG=1 GXV_FEPLAN_CENSUS=100 FEW_MIXED=12 FEW_CONSOLE=1 "${norand[@]}" "$out/few" 120 1 2>&1 >/dev/null | grep "feplan-census" | head -2 | cut -c1-900 | sed 's/^/     /'
[ $fail = 0 ] && echo "PASS: mixed-list plans print the parser's state and command buffers ($seeds seeds, $calls random calls; fe-work $frames pictures; host, not Vita timing)"
exit $fail
