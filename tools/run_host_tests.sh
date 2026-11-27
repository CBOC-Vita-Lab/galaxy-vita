#!/bin/bash
# run_host_tests.sh [-j1] [-t SECONDS] [-f REGEX] [--list] [--orphans]
#   Runs every tools/test_*.sh (each compiles its tests/*.cpp and runs it) ONE AT A TIME under nice, with each script's own compiler default (set CXX to force one).
#   Prints one line per test:  PASS|FAIL|NOBUILD|TIMEOUT name (seconds)   and a summary; exit 0 only when every test passed.
#   NOBUILD = the script printed a build failure (the compiler or linker failed: "FAILED", "error:", exit 2, or an exit code
#   of the compiler); FAIL = it built and then a check failed; TIMEOUT = over -t seconds (default 240).
#   --list      print the scripts that would run          --orphans  list tests/*.cpp|*.c that no tools/test_*.sh or *.py mentions (never run by anything)
#   -f REGEX    only scripts whose name matches
#   SKIP = the script exited 0 and printed a line starting "SKIP" (e.g. "SKIP: no EGL"), or it needs docker and no docker daemon
#   is reachable, or it is listed in tools/host_tests_skip.txt (needs a Vita / emulator / arguments). SKIP does not fail the run.
#   -j N        run N scripts in parallel (default 1)
#   Sanitizers: tools/hosttest_shim (first on PATH) drops -fsanitize=X when the compiler cannot link that runtime; HOSTTEST_SAN=on disables the shim.
#   Results: $RESULTS (default /tmp/host-tests-<pid>.tsv): status, name, seconds, last line of output. Full output of failures: $RESULTS.d/<name>.out
# Why: the tests were run by hand, script by script, so a test that stopped compiling went unseen (a run of "all tests" was nobody's job).
root="$(cd "$(dirname "$0")/.." && pwd)"
TMO=240; JOBS=1; FILT=; LIST=0; ORPH=0
while [ $# -gt 0 ]; do case "$1" in -t) TMO=$2; shift;; -f) FILT=$2; shift;; --list) LIST=1;; --orphans) ORPH=1;; -j1) ;; -j) JOBS=$2; shift;; *) echo "usage: see header"; exit 2;; esac; shift; done
cd "$root" || exit 2
if [ "$ORPH" = 1 ]; then
  n=0
  for t in tests/*.cpp tests/*.c; do
    [ -f "$t" ] || continue
    b=$(basename "$t")
    if ! grep -rqF -- "$b" tools/*.sh tools/*.py tests/*.py tests/*.sh 2>/dev/null; then echo "ORPHAN $t"; n=$((n+1)); fi
  done
  echo "$n test source files are not named by any tools/test_*.sh / script"; exit 0
fi
scripts=$(ls tools/test_*.sh | grep -E -- "${FILT:-.}")
[ "$LIST" = 1 ] && { echo "$scripts"; exit 0; }
export HOSTTEST_REAL_PATH=$PATH HOSTTEST_CACHE=${HOSTTEST_CACHE:-/tmp/hosttest-san-cache-$$}
export PATH="$root/tools/hosttest_shim:$PATH"
RESULTS=${RESULTS:-/tmp/host-tests-$$.tsv}; mkdir -p "$RESULTS.d"; : > "$RESULTS"
skiplist=$(sed 's/#.*//' tools/host_tests_skip.txt 2>/dev/null | awk 'NF{print $1}')
docker_ok=0; docker info >/dev/null 2>&1 && docker_ok=1
run_one() {
  s=$1; name=$(basename "$s" .sh); t0=$(date +%s); o="$RESULTS.d/$name.out"
  if printf '%s\n' "$skiplist" | grep -qxF "$name"; then st=SKIP; echo "SKIP: listed in tools/host_tests_skip.txt" >"$o"; rc=0
  elif [ "$docker_ok" = 0 ] && grep -q 'docker run' "$s"; then st=SKIP; echo "SKIP: needs docker, no daemon" >"$o"; rc=0
  else
    nice -n "${HOSTTEST_NICE:-19}" timeout "$TMO" bash "$s" > "$o" 2>&1; rc=$?
    if [ $rc -eq 124 ]; then st=TIMEOUT
    elif [ $rc -eq 0 ]; then if grep -aq '^SKIP' "$o"; then st=SKIP; else st=PASS; fi
    elif grep -aqE 'build FAILED|error:|undefined reference|fatal error|cannot find|No such file|command not found|clang\+\+: |g\+\+: ' "$o" && ! grep -aqE '^(PASS|ok)' "$o"; then st=NOBUILD
    else st=FAIL; fi
  fi
  dt=$(( $(date +%s) - t0 ))
  [ $st = PASS ] && rm -f "$o"
  printf '%s\t%s\t%s\t%s\n' "$st" "$name" "$dt" "$(tail -n 1 "$o" 2>/dev/null | cut -c1-140)" >> "$RESULTS"
  echo "$st $name (${dt}s)"
}
export -f run_one; export TMO RESULTS skiplist docker_ok
printf '%s\n' $scripts | xargs -P "$JOBS" -I{} bash -c 'run_one {}'
cnt() { grep -c "^$1	" "$RESULTS"; }
total=$(wc -l < "$RESULTS"); pass=$(cnt PASS); skip=$(cnt SKIP); fail=$(cnt FAIL); nob=$(cnt NOBUILD); tmo=$(cnt TIMEOUT)
printf '\n%-8s %s\n' STATUS COUNT; printf '%-8s %s\n' PASS $pass SKIP $skip FAIL $fail NOBUILD $nob TIMEOUT $tmo TOTAL $total
[ $((fail + nob + tmo)) -gt 0 ] && { echo "Not green:"; grep -E "^(FAIL|NOBUILD|TIMEOUT)	" "$RESULTS" | cut -f1,2,4; }
echo "(details: $RESULTS, outputs: $RESULTS.d)"
[ $((fail + nob + tmo)) -eq 0 ] && [ "$total" -gt 0 ]
