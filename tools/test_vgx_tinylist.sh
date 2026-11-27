#!/usr/bin/env bash
# Tiny state lists through the state-list cache (GXV_TINYLIST, platform/vgx/src/vgx_tinylist.inc) and the tiny branch's census
# (GXV_TINYLIST_CENSUS) on the host. There are no recorded GX captures of the game in the repository, so the two fixtures are:
#   1. tests/vgx_tinylist.cpp: random tiny lists (J3D's small differed lists, XF / BP / CP / indexed-load-first state lists, padded or not,
#      aligned or not, masks, NOPs, a draw-sync token, state with a draw, particle quads, draws only, a CALL_DL inside, an unknown opcode, a cut
#      command), called through CallList, rewritten with the same bytes / changed / written next to / made again, the matrices an indexed
#      load reads changed; a hash of the State after every call, every token, and of every command buffer.
#   2. tests/vgx_fe_work.cpp FEW_CONSOLE=1: the J3D-shaped scene with the console's share of J3D's small differed lists (XF first, 32 / 64
#      bytes), tiny geometry lists, particles (FEW_JPA) and the other switches around it.
# Passes when, in every setting of the switches around it, GXV_TINYLIST=1 and =2 print exactly what GXV_TINYLIST=0 prints; =1 replays
# tiny lists (and meets refusals and back-offs); =2 checks replays with 0 mismatching and leaves the token lists uncompared; the census
# prints its lines and changes nothing; and the negative controls fail as they must: -DGXV_TINYLIST_MUTANT=1 (a list replayed without its
# bytes being checked) and =2 (the replay made of the first 32 bytes) make =1 differ from =0, and =2 report mismatches while it keeps the
# parser's output. REF_ROOT=<a source tree without the change> (optional): fe-work built against that tree's front end must print what this
# tree prints with every switch at its default, and with the census on.
#   tools/test_vgx_tinylist.sh [calls] [seeds] [fe-work frames]     (CXX: default g++; build in $OUT, default a directory of tests/mp/tmproot.sh)
set -euo pipefail
. "$(cd "$(dirname "$0")" && pwd)/../tests/mp/tmproot.sh"
root=$(cd "$(dirname "$0")/.." && pwd)
calls=${1:-6000}
seeds=${2:-"1 2 3"}
frames=${3:-60}
out=${OUT:-$MP_TMP/vgx_tinylist}
mkdir -p "$out"
cxx=${CXX:-g++}
cd "$root"
lib=(platform/vgx/src/vgx_draw.cpp platform/vgx/src/vgx_fesplit.cpp platform/vgx/src/vgx_dlcache.cpp platform/vgx/src/vgx_guestmem.cpp
     platform/vgx/src/vgx_bpsync.cpp platform/vgx/src/vgx_shadergen.cpp platform/vgx/src/vgx_texcache.cpp platform/vgx/src/vgx_texdecode.cpp
     platform/vgx/src/vgx_texcomp.cpp platform/vgx/src/vgx_stats.cpp platform/vgx/src/vgx_backend_null.cpp)
common=(-std=c++17 -g -DVGX_NO_NEON -Iplatform/vgx/src -Iplatform/include -Iplatform/vgx/third_party -pthread)
san=(-O1 -fsanitize=address,undefined -fno-omit-frame-pointer)
objs() { # objs <dir> <flags...>: the front end's other units, once per flavour (rebuilt when the source is newer)
  local d=$1; shift; mkdir -p "$d"
  for f in "${lib[@]}"; do local o="$d/$(basename "$f" .cpp).o"; [ -s "$o" ] && [ "$o" -nt "$f" ] || $cxx "${common[@]}" "$@" -c "$f" -o "$o"; done
}
objs "$out/san" "${san[@]}"
objs "$out/o2" -O2
$cxx "${common[@]}" "${san[@]}" -c tests/vgx_tinylist.cpp -o "$out/san/test.o"
for v in "" 1 2; do # the state unit: plain, and the two negative controls
  def=(); [ -n "$v" ] && def=(-DGXV_TINYLIST_MUTANT=$v)
  $cxx "${common[@]}" "${san[@]}" "${def[@]}" -c platform/vgx/src/vgx_state.cpp -o "$out/san/state$v.o"
  $cxx "${common[@]}" "${san[@]}" "$out/san/test.o" "$out/san/state$v.o" "$out"/san/vgx_*.o -o "$out/tinylist$v"
done
$cxx "${common[@]}" -O2 -c platform/vgx/src/vgx_state.cpp -o "$out/o2/state.o"
$cxx "${common[@]}" -O2 -c tests/vgx_fe_work.cpp -o "$out/o2/few.o"
$cxx "${common[@]}" -O2 "$out/o2/few.o" "$out/o2/state.o" "$out"/o2/vgx_*.o -o "$out/few"
if [ -n "${REF_ROOT:-}" ]; then
  mkdir -p "$out/ref"
  for f in "${lib[@]}" platform/vgx/src/vgx_state.cpp; do
    $cxx -std=c++17 -g -DVGX_NO_NEON -I"$REF_ROOT/platform/vgx/src" -I"$REF_ROOT/platform/include" -Iplatform/vgx/third_party -pthread -O2 -c "$REF_ROOT/$f" -o "$out/ref/$(basename "$f" .cpp).o"
  done
  $cxx -std=c++17 -g -DVGX_NO_NEON -I"$REF_ROOT/platform/vgx/src" -I"$REF_ROOT/platform/include" -Iplatform/vgx/third_party -pthread -O2 -c tests/vgx_fe_work.cpp -o "$out/ref/few.o"
  $cxx -pthread "$out/ref/few.o" "$out"/ref/vgx_*.o -o "$out/few-ref"
fi
export ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
# (the State holds host pointers into the fixture's memory: runs are compared with address randomisation off)
norand=(); if setarch "$(uname -m)" -R true 2>/dev/null; then norand=(setarch "$(uname -m)" -R); fi
fail=0
num() { sed -n "s/.*$1.*/\\1/p" "$2" | head -1; }
# ---- 1. random tiny lists
for seed in $seeds; do
  for around in "" "GXV_SLDELTA=0" "GXV_SLDELTA=2" "GXV_BPFAST=0" "GXV_REGSHADOW=0" "GXV_SLBYTES=0" "GXV_SLFAST=0" "GXV_STATELIST=0" "GXV_STATELIST=2" \
                "GXV_FECUT_SLHOT=1" "GXV_FEPLAN=1" "GXV_FEPLAN_CENSUS=1" "GXV_DL_TINY=32" "GXV_FE_STATEFAST=1" "GXV_NOOP_FILTER=1" "GXV_TINYLIST_CENSUS=1"; do
    env $around GXV_TINYLIST=0 "${norand[@]}" "$out/tinylist" "$calls" "$seed" > "$out/p0.txt" 2> "$out/p0.err" || { echo "FAIL seed $seed [$around] TINYLIST=0 crashed"; tail -5 "$out/p0.err"; fail=1; continue; }
    for m in 1 2; do
      env $around GXV_TINYLIST=$m "${norand[@]}" "$out/tinylist" "$calls" "$seed" > "$out/p$m.txt" 2> "$out/p$m.err" || { echo "FAIL seed $seed [$around] TINYLIST=$m crashed"; tail -5 "$out/p$m.err"; fail=1; continue; }
      if ! cmp -s "$out/p0.txt" "$out/p$m.txt"; then
        echo "FAIL seed $seed [$around]: GXV_TINYLIST=$m differs from the parser: $(diff "$out/p0.txt" "$out/p$m.txt" | head -2 | tr '\n' ' ')"; fail=1
      fi
    done
    fr=$(num 'tinylist: \([0-9]*\) fast replays' "$out/p1.err"); sr=$(num 'fast replays, \([0-9]*\) StateListCall' "$out/p1.err")
    rf=$(num 'replays, \([0-9]*\) refused' "$out/p1.err"); bo=$(num 'refused, \([0-9]*\) backed off' "$out/p1.err")
    ck=$(num 'verify: \([0-9]*\) checked' "$out/p2.err"); bad=$(num 'checked, \([0-9]*\) mismatching' "$out/p2.err"); tr=$(num 'not compared \([0-9]*\) trigger' "$out/p2.err")
    if [ "$around" = "GXV_STATELIST=0" ]; then # (no state lists at all: every candidate refused, parsed)
      if [ "${fr:-1}" != 0 ] || [ "${sr:-1}" != 0 ] || [ "${bad:-1}" != 0 ]; then echo "FAIL seed $seed [$around]: replays ${fr:-?} + ${sr:-?}, mismatching ${bad:-?}"; fail=1; fi
    elif [ $(( ${fr:-0} + ${sr:-0} )) -lt 1500 ] || [ "${sr:-0}" -lt 20 ] || [ "${rf:-0}" -lt 1 ] || [ "${bo:-0}" -lt 50 ] || [ "${ck:-0}" -lt 500 ] || [ "${bad:-1}" != 0 ] || [ "${tr:-0}" -lt 1 ]; then
      echo "FAIL seed $seed [$around]: fast ${fr:-?}, StateListCall ${sr:-?}, refused ${rf:-?}, backed off ${bo:-?}; verify checked ${ck:-?}, mismatching ${bad:-?}, trigger ${tr:-?}"; fail=1
    fi
    [ -z "$around" ] && echo "     seed $seed: $(tail -1 "$out/p1.err" | cut -c1-260)"
  done
  # census lines (log only: the output must not change)
  GXV_TINYLIST_CENSUS=50 TINYLIST_LOG=1 GXV_TINYLIST=0 "${norand[@]}" "$out/tinylist" "$calls" "$seed" > "$out/c0.txt" 2> "$out/c0.err"
  GXV_TINYLIST_CENSUS=50 TINYLIST_LOG=1 GXV_TINYLIST=1 "${norand[@]}" "$out/tinylist" "$calls" "$seed" > "$out/c1.txt" 2> "$out/c1.err"
  GXV_TINYLIST=0 "${norand[@]}" "$out/tinylist" "$calls" "$seed" > "$out/p0.txt" 2> /dev/null
  if ! grep -q "tinylist-census: .* could replay (state only, same bytes, parsed) [1-9]" "$out/c0.err" || ! grep -q "tinylist-census: costliest" "$out/c0.err" || \
     ! grep -q "GXV_TINYLIST replayed [1-9]" "$out/c1.err" || ! cmp -s "$out/c0.txt" "$out/p0.txt" || ! cmp -s "$out/c1.txt" "$out/p0.txt"; then
    echo "FAIL seed $seed: census lines missing or the census changed the output"; fail=1
  fi
  # negative controls
  for v in 1 2; do
    GXV_TINYLIST=0 "${norand[@]}" "$out/tinylist$v" "$calls" "$seed" > "$out/m0.txt" 2> /dev/null || true # (its own reference: the State holds host addresses, which another binary places elsewhere)
    GXV_TINYLIST=1 "${norand[@]}" "$out/tinylist$v" "$calls" "$seed" > "$out/m1.txt" 2> /dev/null || true
    GXV_TINYLIST=2 "${norand[@]}" "$out/tinylist$v" "$calls" "$seed" > "$out/m2.txt" 2> "$out/m2.err" || true
    mbad=$(num 'checked, \([0-9]*\) mismatching' "$out/m2.err")
    if cmp -s "$out/m0.txt" "$out/m1.txt"; then echo "FAIL seed $seed: mutant $v: GXV_TINYLIST=1 did not change the output (the test cannot see it)"; fail=1; fi
    if [ "${mbad:-0}" -lt 1 ]; then echo "FAIL seed $seed: mutant $v: the verify mode saw no mismatch"; fail=1; fi
    if ! cmp -s "$out/m0.txt" "$out/m2.txt"; then echo "FAIL seed $seed: mutant $v: GXV_TINYLIST=2 did not keep the parser's output"; fail=1; fi
    echo "     seed $seed: mutant $v caught (=1 differs; verify mismatching ${mbad:-?})"
  done
done
# ---- 2. the J3D-shaped scene with the console's tiny differed lists
for seed in $seeds; do
  for around in "" "GXV_SPLIT=1" "GXV_SPLIT=2" "GXV_SLDELTA=0" "GXV_SLDELTA=2" "GXV_FE_STATEFAST=1" "GXV_RETAINED=1 FEW_RETAINED=1" "FEW_JPA=60" \
                "FEW_JPA=60 FEW_FXMERGE=1 GXV_FXMERGE=1" "FEW_KONST=1" "GXV_FEPLAN=1 FEW_MIXED=12" "GXV_TINYLIST_CENSUS=1" "GXV_WORKLOAD_PROBE=1 GXV_FE_SPLIT=2"; do
    env $around FEW_CONSOLE=1 GXV_TINYLIST=0 "${norand[@]}" "$out/few" "$frames" "$seed" > "$out/w0.txt" 2> "$out/w0.err"
    for m in 1 2; do
      env $around FEW_CONSOLE=1 GXV_TINYLIST=$m "${norand[@]}" "$out/few" "$frames" "$seed" > "$out/w$m.txt" 2> "$out/w$m.err"
      if ! cmp -s "$out/w0.txt" "$out/w$m.txt"; then echo "FAIL fe-work seed $seed [$around]: GXV_TINYLIST=$m differs"; diff "$out/w0.txt" "$out/w$m.txt" | head -2; fail=1; fi
    done
    r1=$(num 'tinylist: \([0-9]*\) fast replays' "$out/w1.err"); ck=$(num 'tinylist: .*verify: \([0-9]*\) checked' "$out/w2.err"); bad=$(num 'tinylist: .*checked, \([0-9]*\) differing' "$out/w2.err")
    if [ "${r1:-0}" -lt 100 ] || [ "${ck:-0}" -lt 100 ] || [ "${bad:-1}" != 0 ]; then echo "FAIL fe-work seed $seed [$around]: replays ${r1:-?}, checked ${ck:-?}, differing ${bad:-?}"; fail=1; fi
    [ -z "$around" ] && echo "     fe-work seed $seed: $(grep 'tinylist:' "$out/w1.err" | cut -c1-200)"
  done
  # the scene without the console's tiny lists: the switch on changes nothing there either
  GXV_TINYLIST=0 "${norand[@]}" "$out/few" "$frames" "$seed" > "$out/n0.txt" 2> /dev/null
  GXV_TINYLIST=1 "${norand[@]}" "$out/few" "$frames" "$seed" > "$out/n1.txt" 2> /dev/null
  cmp -s "$out/n0.txt" "$out/n1.txt" || { echo "FAIL fe-work seed $seed: TINYLIST=1 changed the scene without tiny differed lists"; fail=1; }
  if [ -n "${REF_ROOT:-}" ]; then
    for around in "" "FEW_CONSOLE=1" "FEW_CONSOLE=1 GXV_SPLIT=1" "FEW_CONSOLE=1 FEW_JPA=60" "GXV_SPLIT=1"; do
      env $around "${norand[@]}" "$out/few-ref" "$frames" "$seed" > "$out/ref.txt" 2> /dev/null
      env $around "${norand[@]}" "$out/few" "$frames" "$seed" > "$out/now.txt" 2> /dev/null
      env $around GXV_TINYLIST_CENSUS=1 "${norand[@]}" "$out/few" "$frames" "$seed" > "$out/nowc.txt" 2> /dev/null
      if ! cmp -s "$out/ref.txt" "$out/now.txt" || ! cmp -s "$out/ref.txt" "$out/nowc.txt"; then
        echo "FAIL fe-work seed $seed [$around]: the defaults (or the census) differ from the front end of $REF_ROOT"; fail=1
      fi
    done
    echo "     fe-work seed $seed: defaults and census identical to $REF_ROOT (5 settings)"
  fi
done
FEW_LOG=1 GXV_TINYLIST_CENSUS=100 FEW_CONSOLE=1 FEW_JPA=60 "${norand[@]}" "$out/few" 120 1 2>&1 >/dev/null | grep "tinylist-census" | head -2 | cut -c1-1200 | sed 's/^/     /'
[ $fail = 0 ] && echo "PASS: tiny state lists through the state-list cache print the parser's state, tokens and command buffers ($seeds seeds, $calls random calls; fe-work $frames pictures; host, not Vita timing)"
exit $fail
