#!/usr/bin/env bash
# GXV_JPA_LOOK_PURE / GXV_PTCL_LOOKPAR host check (gamecpu23, 8 Nov): builds parwork's particle test (tools/test_par_ptcl.sh: the real JPA units,
# a real second thread) and runs it with world S = the ORIGINAL calc (GXV_JPA_LOOK off: every look function in every calc) against world P =
# GXV_JPA_LOOK + GXV_JPA_LOOK_PURE (+ GXV_PTCL_LOOKPAR, parallel turns), with a picture simulated before every drawn turn (the draw's look
# flush); the worlds must be byte-identical after every picture, for pictures every turn (60), every second turn (30) and irregular ones.
# The negative control (P's picture without its look flush) must FAIL.
#   bash tools/test_jpa_look_pure.sh [out-dir]
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d "${TMPDIR:-/tmp}/jpa-look-pure.XXXXXX")}
mkdir -p "$out"
bash "$root/tools/test_par_ptcl.sh" "$out" > "$out/build.log" 2>&1 || { tail -5 "$out/build.log"; echo "jpa_look_pure: FAIL (the base test)"; exit 1; }
fail=0
run() {
    local r
    r=$(env "$@" "$out/par_ptcl" | tail -2)
    echo "[$*] $r" | tr '\n' ' '; echo
    case "$r" in *"par_ptcl: PASS"*) ;; *) fail=1 ;; esac
}
for pat in 60 30 rand; do
    run PTCL_S_ORIG=1 PTCL_LOOKPURE=1 PTCL_DRAWPAT=$pat
    run PTCL_S_ORIG=1 PTCL_LOOKPURE=1 PTCL_LOOKPAR=1 PTCL_DRAWPAT=$pat
    run PTCL_S_ORIG=1 PTCL_LOOKPURE=1 PTCL_LOOKPAR=2 PTCL_DRAWPAT=$pat
    run PTCL_S_ORIG=1 PTCL_LOOKPURE=2 PTCL_DRAWPAT=$pat
    run PTCL_OVERLAP=1 PTCL_S_ORIG=1 PTCL_LOOKPURE=1 PTCL_LOOKPAR=1 PTCL_DRAWPAT=$pat
done
neg=$( (PTCL_NEG_NOFLUSH=1 PTCL_S_ORIG=1 PTCL_LOOKPURE=1 PTCL_DRAWPAT=30 "$out/par_ptcl" || true) | tail -1)
echo "[negative control] $neg"
case "$neg" in *"par_ptcl: FAIL"*) ;; *) echo "the negative control did not fail"; fail=1 ;; esac
if [ $fail = 0 ]; then echo "jpa_look_pure: PASS"; else echo "jpa_look_pure: FAIL"; exit 1; fi
