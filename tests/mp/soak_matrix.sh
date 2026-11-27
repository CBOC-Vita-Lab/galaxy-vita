#!/bin/bash
# The exactly-once soak over a matrix (netcore):
#   tests/mp/soak_matrix.sh <soak_exact binary> <number of seeds> "<backoff shifts or ->" "<g_creep values or ->"
# e.g. tests/mp/soak_matrix.sh /tmp/mp_core_tests/soak_exact 20 "1 2 3 4" "0 1 2"
# One SOAK line per run (seed, creep, shift, dups, gaps, lost events, tail lost, convergence ...), then one summary line per cell.
# Each run is ~0.1 s at -O2 and ~2 s under ASan (2026-11 container).
set -o pipefail
bin=$1; seeds=$2; shifts=${3:--}; creeps=${4:--}
tab=$(mktemp)
for sh in $shifts; do for cr in $creeps; do for s in $(seq 0 $((seeds-1))); do
  a="-seed $s"; [ "$sh" != "-" ] && a="$a -shift $sh"; [ "$cr" != "-" ] && a="$a -creep $cr"
  # (a crash / timeout / non-zero exit of the soak is a FAIL cell, not nothing: it used to be `|| true`, and a run that died printed no line at all)
  rc=0; o=$(nice -n 10 timeout 300 "$bin" $a 2>/dev/null) || rc=$?
  [ -n "$o" ] && printf '%s\n' "$o"
  if [ $rc != 0 ] && ! printf '%s\n' "$o" | tail -1 | grep -q 'FAIL$'; then echo "SOAK shift=$sh creep=$cr seed=$s exit=$rc FAIL"; fi
done; done; done | tee /dev/stderr | awk '{for(i=1;i<=NF;i++){split($i,kv,"="); v[kv[1]]=kv[2]} key="shift="v["shift"]" creep="v["creep"]; n[key]++;
  if($NF=="FAIL"){f[key]++; ff[key]=ff[key]" "v["seed"]}} END{for(k in n) printf "CELL %s runs=%d fail=%d%s\n",k,n[k],f[k]+0,(f[k]?" seeds:"ff[k]:"")}' | sort | tee "$tab"
! grep -q 'fail=[1-9]' "$tab" || { rm -f "$tab"; exit 1; }   # (the exit status says it: the table above used to be the only signal)
[ -s "$tab" ] || { rm -f "$tab"; echo "soak_matrix: no cell ran"; exit 1; }
rm -f "$tab"
