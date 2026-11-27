#!/bin/bash
# run_all_host_tests.sh [--quick] [--list] [-f REGEX] [-t SECONDS] [--out DIR] [--write-quick] [--no-skip-probe]
#
# ONE command for every host test suite of the repo that runs without private game data, a console, an emulator or a game build:
#   tools      tools/test_*.sh (each compiles its tests/*.cpp and runs it) and the loose tools/test_*.py / test_*.mjs
#              The C182..C185 chains (tools/test_c182.sh .. test_c185.sh) are EXPANDED: each step is a row of its own (a chain stops at its first
#              failure and hides the rest); what only the chains ran (title_cloud, c182_pair_depth) is the row "c18x-chain-extras".
#   python     python3 -m unittest discover -s tests
#   mp         tests/mp/build.sh (core tests, fuzz, 8 soak seeds, friend code), every tests/mp/*_build.sh and *_test.sh, tests/mp/ui_menu/build.sh,
#              tools/mp/partner/test.sh quick
#   net        net/tests/run_online_tests.sh, run_signal_http_test.sh (against the local fake service), run_https_test.sh
#   server     npm test in server/ (vitest; needs node_modules)
#   misc       tests/pgo_host/*.sh, tests/speed31_offhash.sh
# Left out on purpose (named in --list): tests/mp/emu_all.sh (starts the emulator), tests/mp/keepsafe_findings.sh (an audit that fails while its findings are
# open), tests/mp/rev_guard_mutations.sh (needs an argument), tests/mp/soak_matrix.sh and tsan_* (long, run by hand), anything that needs private game data.
#
# One summary table at the end: suite, result, seconds, note.  Results:
#   PASS            built and ran clean (the note says "NO SANITIZERS" when a suite had to build without them; HOST_SANITIZE=1 makes that an error)
#   FAIL            a check failed, the build failed ("FAIL (build)") or the suite ran out of time ("FAIL (timeout)"): the exit code is non-zero
#   SKIP <reason>   cannot run on THIS host for an environment reason (no docker, no EGL, no node_modules, ...): never counted as a failure, always printed
# A script may also skip itself by exiting 77 (its last output line is the reason).
# Exit code: 0 when every suite that could run passed, 1 when one failed, 2 on a usage error.
#
#   --quick        the fast subset: the suites named in tools/host_tests_quick.txt (about 10 minutes on the 2-core builder, see --write-quick)
#   --list         print the suites (and what a skip would say) without running anything
#   -f REGEX       only the suites whose name matches        -t SECONDS  per-suite time limit (default 900)
#   --out DIR      results (default /tmp/host-tests-all-<tree>-<time>): results.tsv and <suite>.log of every suite that did not pass
#   --write-quick  after a full run, rewrite tools/host_tests_quick.txt from this run: PASS suites of at most QUICK_MAX_S (8) seconds, until QUICK_BUDGET_S (420)
# Compilers: each suite picks its own (the scripts call tools/host_san.sh: clang++ with sanitizers when its runtime exists, else g++ with them, else plain, and
# say so); the vgx back end suites need clang++ (g++ 13 refuses its extern "C" port_scene_serial). This runner prints which sanitizer runtimes the host has.
# Runs one suite at a time under nice -n 10. Never starts the emulator, a game build, docker or a live service.
root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root" || exit 2
QUICK=0; LIST=0; FILT=; TMO=900; OUT=; WRITEQ=0
while [ $# -gt 0 ]; do
  case "$1" in
    --quick) QUICK=1;; --list) LIST=1;; -f) FILT=$2; shift;; -t) TMO=$2; shift;; --out) OUT=$2; shift;; --write-quick) WRITEQ=1;;
    -h|--help) sed -n 2,35p "$0"; exit 0;; *) echo "usage: $0 [--quick] [--list] [-f REGEX] [-t SECONDS] [--out DIR] [--write-quick]  (see the header)"; exit 2;;
  esac; shift
done
QUICK_FILE=tools/host_tests_quick.txt
CHAINS="test_c182 test_c183 test_c184 test_c185"

# ---------------------------------------------------------------- the catalogue: id <TAB> group <TAB> command
cat_rows() {
  local f n
  for f in tools/test_*.sh; do
    n=$(basename "$f" .sh)
    case " $CHAINS " in *" $n "*) continue;; esac
    printf '%s\ttools\tbash %s\n' "$n" "$f"
  done
  printf 'c18x-chain-extras\ttools\tbash tools/run_all_host_tests.sh --chain-extras\n'
  for f in tools/test_*.py; do [ -f "$f" ] && printf '%s\ttools\tpython3 %s\n' "$(basename "$f")" "$f"; done
  for f in tools/test_*.mjs; do [ -f "$f" ] && printf '%s\ttools\tnode %s\n' "$(basename "$f")" "$f"; done
  printf 'python-unittest-tests\tpython\tpython3 -m unittest discover -s tests\n'
  printf 'mp/build\tmp\tbash tests/mp/build.sh\n'
  for f in tests/mp/*_build.sh tests/mp/*_test.sh; do
    n=$(basename "$f" .sh)
    case "$n" in tsan_build|soak_build) continue;; esac   # (tsan: by hand; soak_build: a soak matrix builder, covered by mp/build's soak_exact seeds)
    printf 'mp/%s\tmp\tbash %s\n' "$n" "$f"
  done
  printf 'mp/ui_menu\tmp\tbash tests/mp/ui_menu/build.sh\n'
  printf 'mp/partner\tmp\tbash tools/mp/partner/test.sh quick\n'
  printf 'net/online\tnet\tNET_SAN=1 bash net/tests/run_online_tests.sh \"$OUTD/net-online\"\n'
  printf 'net/signal_http\tnet\tNET_SAN=1 bash net/tests/run_signal_http_test.sh \"$OUTD/net-signal\"\n'
  printf 'net/https\tnet\tbash net/tests/run_https_test.sh \"$OUTD/net-https\"\n'
  printf 'server/npm-test\tserver\tcd server && npm test\n'
  printf 'pgo_host/run_test\tmisc\tbash tests/pgo_host/run_test.sh \"$OUTD/pgo-host\"\n'
  printf 'pgo_host/run_test32\tmisc\tbash tests/pgo_host/run_test32.sh \"$OUTD/pgo-host32\"\n'
  printf 'pgo_host/run_synth\tmisc\tbash tests/pgo_host/run_synth.sh \"$OUTD/pgo-synth\"\n'
  printf 'speed31_offhash\tmisc\tbash tests/speed31_offhash.sh\n'
}

# ---------------------------------------------------------------- what the chains ran that no single script runs
if [ "${1:-}" = "--chain-extras" ]; then
  . tools/host_san.sh
  t=$(mktemp -d); trap 'rm -rf "$t"' EXIT
  cxx=${CXX:-c++}
  san=($(host_san "$cxx" -fsanitize=address,undefined))
  "$cxx" -std=c++17 ${san[@]+"${san[@]}"} -Iplatform/include tests/title_cloud.cpp -o "$t/title-cloud" && "$t/title-cloud" || exit 1   # (test_c183.sh)
  "$cxx" -std=c++17 ${san[@]+"${san[@]}"} -Iplatform/vgx/include -Iplatform/vgx/src tests/c182_pair_depth.cpp -o "$t/pair-depth" && "$t/pair-depth" || exit 1   # (test_c182.sh)
  exit 0
fi

# ---------------------------------------------------------------- host facts
have() { command -v "$1" >/dev/null 2>&1; }
docker_ok() { have docker && timeout 10 docker info >/dev/null 2>&1; }
asan_ok() { # <compiler>
  local d; d=$(mktemp -d); printf 'int main(){return 0;}\n' > "$d/t.cpp"
  if "$1" -fsanitize=address,undefined "$d/t.cpp" -o "$d/t" >/dev/null 2>&1 && "$d/t" >/dev/null 2>&1; then rm -rf "$d"; return 0; fi
  rm -rf "$d"; return 1
}
egl_ok() { [ -f /usr/include/EGL/egl.h ] && [ -f /usr/include/GLES2/gl2.h ] && { ldconfig -p 2>/dev/null | grep -q 'libEGL\.so' ; }; }
HAVE_DOCKER=0; docker_ok && HAVE_DOCKER=1
HAVE_EGL=0; egl_ok && HAVE_EGL=1
CLANG_ASAN=no; have clang++ && asan_ok clang++ && CLANG_ASAN=yes
GXX_ASAN=no; have g++ && asan_ok g++ && GXX_ASAN=yes
NODE=${NODE:-$(command -v node || echo /opt/node22/bin/node)}
export NODE
# Compilers: every script keeps its own choice (CXX unset: some default to clang++, some to g++, some probe with tools/host_san.sh). Where clang has no sanitizer
# runtime of its own (this builder), a shim directory in front of PATH makes `clang++` / `clang` tools/host_cxx.sh: plain pass-through, except that a LINK with
# -fsanitize=address/undefined uses clang's instrumentation with GCC's libasan/libubsan (the decomp code needs clang: g++ 13 rejects it). An exported CXX is
# respected. The mp/ and net/ suites were written for g++ + sanitizers where clang has none and get CXX=g++. The vgx back end suites use clang++ whatever CXX
# says (host_backend_cxx in tools/host_san.sh).
NET_SAN_CXX=; NET_SAN_CC=; HOSTCXX_SAN=no
if [ "$CLANG_ASAN" = yes ]; then NET_SAN_CXX=clang++; NET_SAN_CC=clang; elif [ "$GXX_ASAN" = yes ]; then NET_SAN_CXX=g++; NET_SAN_CC=gcc; fi
if [ "$CLANG_ASAN" = no ] && have clang++ && bash tools/host_cxx.sh --probe; then
  HOSTCXX_SAN=yes; SHIM=${OUT:-/tmp}/.host-shim-$$; mkdir -p "$SHIM"; export HOST_CXX_REAL_DIR=$(dirname "$(command -v clang++)")
  for n in clang++ clang; do ln -sf "$root/tools/host_cxx.sh" "$SHIM/$n"; done
  export PATH="$SHIM:$PATH"; trap 'rm -rf "$SHIM"' EXIT
fi

# A reason when the suite cannot run on this host (checked before it starts; empty = run it).
skip_reason() { # <id> <group> <cmd>
  local id=$1 cmd=$3 script text
  script=$(printf '%s' "$cmd" | sed -n 's/^\(NET_SAN=1 \)\?bash \([^ ]*\).*/\2/p')
  [ -n "$script" ] && [ -f "$script" ] && text=$(cat "$script")
  case "$id" in
    server/npm-test) [ -x server/node_modules/.bin/vitest ] || { echo "needs npm install (server/node_modules is absent; vitest + wrangler are not installed, and this runner does not fetch them)"; return; };;
    net/https)
      [ -d "${MBEDTLS:-/tls/mbedtls/host64}/include" ] || { echo "needs mbedtls host build (${MBEDTLS:-/tls/mbedtls/host64})"; return; }
      python3 -c 'import cryptography' 2>/dev/null || { echo "needs python3 cryptography (for the test CA)"; return; };;
    pgo_host/*) have llvm-profdata-18 || have llvm-profdata || { echo "needs llvm-profdata 18"; return; };;
    mp/service_e2e_test|mp/service_test|net/signal_http) "$NODE" -e 'process.exit(+(process.versions.node.split(".")[0]<22))' 2>/dev/null || { echo "needs node >= 22.5"; return; };;
  esac
  if [ -n "${text:-}" ] && ! printf '%s' "$text" | grep -q '^# host-fallback:'; then   # (a script with that marker runs without docker / qemu by itself)
    if [ $HAVE_DOCKER = 0 ] && printf '%s' "$text" | grep -qE '^[^#]*docker (run|image|info)'; then echo "needs docker (no usable docker daemon here)"; return; fi
    if [ $HAVE_EGL = 0 ] && printf '%s' "$text" | grep -qE '^[^#]*-lEGL'; then echo "needs EGL (Mesa EGL/GLESv2 headers and libraries are not installed)"; return; fi
    if printf '%s' "$text" | grep -qE '^[^#]*(qemu-arm|arm-linux-gnueabihf-)' && ! { have qemu-arm && have arm-linux-gnueabihf-g++; }; then echo "needs qemu-arm and arm-linux-gnueabihf-g++"; return; fi
  fi
}

# A reason from a FAILED run's output when the failure is the host's, not the code's (tight patterns; the reason is printed, the log is kept).
env_reason() { # <logfile>
  local l=$1
  grep -aqE 'Cannot connect to the Docker daemon|failed to connect to the docker API|docker: command not found' "$l" && { echo "needs docker"; return; }
  grep -aqE 'EGL/egl\.h: No such file|GLES2/gl2\.h: No such file|cannot find -lEGL|cannot find -lGLESv2' "$l" && { echo "needs EGL"; return; }
  grep -aqE '(qemu-arm|arm-linux-gnueabihf-[a-z+]+): (command )?not found' "$l" && { echo "needs qemu-arm / arm-linux-gnueabihf toolchain"; return; }
  grep -aqiE 'needs? (the )?(private )?game data|game data (is )?(required|missing|not found)|GAME_DIR (is )?(required|not set)' "$l" && { echo "needs game data"; return; }
  return 1
}

# ---------------------------------------------------------------- choose
OUTD=${OUT:-/tmp/host-tests-all-$(basename "$root")-$(date +%m%d-%H%M%S)}
rows=$(cat_rows)
[ -n "$FILT" ] && rows=$(printf '%s\n' "$rows" | awk -F'\t' -v re="$FILT" '$1 ~ re')
if [ $QUICK = 1 ]; then
  [ -f "$QUICK_FILE" ] || { echo "no $QUICK_FILE yet: run a full pass with --write-quick first"; exit 2; }
  rows=$(printf '%s\n' "$rows" | awk -F'\t' 'NR==FNR { if ($0 !~ /^#/ && $0 != "") q[$1]=1; next } ($1 in q)' "$QUICK_FILE" -)
fi
total=$(printf '%s\n' "$rows" | grep -c .)

echo "== compilers: each script keeps its own; clang++ links -fsanitize with the GCC runtime through the tools/host_cxx.sh shim: $HOSTCXX_SAN (clang native runtime: $CLANG_ASAN, g++ runtime: $GXX_ASAN)"
echo "== host: sanitizer runtime: clang++ $CLANG_ASAN, g++ $GXX_ASAN; docker $([ $HAVE_DOCKER = 1 ] && echo yes || echo no); EGL $([ $HAVE_EGL = 1 ] && echo yes || echo no); node $("$NODE" -v 2>/dev/null || echo none); python $(python3 -V 2>&1 | cut -d' ' -f2)"
if [ "$CLANG_ASAN" = no ] && [ "$GXX_ASAN" = no ]; then echo "== NO sanitizer runtime on this host: every suite builds WITHOUT sanitizers (HOST_SANITIZE=1 would make that an error)"
elif [ "$CLANG_ASAN" = no ] && [ "$HOSTCXX_SAN" = yes ]; then echo "== clang++ has no ASan/UBSan runtime of its own here: clang links use GCC's libasan/libubsan through tools/host_cxx.sh; g++ suites use g++'s; the vgx back end builds with clang++ and no sanitizers"
elif [ "$CLANG_ASAN" = no ]; then echo "== clang++ has no ASan/UBSan runtime here and the GCC-runtime shim does not work: suites that use tools/host_san.sh fall back to g++ with sanitizers; clang-only suites build plain"; fi
if [ $LIST = 1 ]; then
  while IFS=$'\t' read -r id grp cmd; do
    [ -z "$id" ] && continue
    r=$(skip_reason "$id" "$grp" "$cmd"); printf '%-8s %-44s %s\n' "$grp" "$id" "${r:+(would SKIP: $r)}"
  done <<< "$rows"
  echo "== $total suites ($([ $QUICK = 1 ] && echo quick || echo full)); chains expanded: $CHAINS; left out: tests/mp/emu_all.sh keepsafe_findings.sh rev_guard_mutations.sh soak_matrix.sh tsan_build.sh tsan_run.sh soak_build.sh"
  exit 0
fi
mkdir -p "$OUTD"; export OUTD
: > "$OUTD/results.tsv"
pass=0; fail=0; skip=0; n=0; T00=$(date +%s)
declare -a TAB
while IFS=$'\t' read -r id grp cmd; do
  [ -z "$id" ] && continue
  n=$((n+1)); safe=$(printf '%s' "$id" | tr '/ ' '__'); log="$OUTD/$safe.log"
  reason=$(skip_reason "$id" "$grp" "$cmd")
  if [ -n "$reason" ]; then
    st="SKIP"; note="$reason"; dt=0; skip=$((skip+1))
  else
    t0=$(date +%s.%N)
    extra=
    case "$id" in mp/*|net/*) [ "$GXX_ASAN" = yes ] && [ "$CLANG_ASAN" = no ] && extra="CXX=g++ CC=gcc";; esac
    case "$id" in net/*) [ -n "$NET_SAN_CXX" ] && extra="SAN='-fsanitize=address,undefined -fno-sanitize-recover=undefined' CXX=$NET_SAN_CXX CC=$NET_SAN_CC";; esac
    ( eval "$extra nice -n 10 timeout $TMO bash -c $(printf '%q' "$cmd")" ) > "$log" 2>&1; rc=$?
    dt=$(awk -v a="$t0" -v b="$(date +%s.%N)" 'BEGIN{printf "%.0f", b-a}')
    note=
    if [ $rc -eq 0 ]; then st=PASS; pass=$((pass+1))
    elif [ $rc -eq 77 ]; then st=SKIP; note="$(tail -n 1 "$log" | cut -c1-100)"; skip=$((skip+1))
    elif [ $rc -eq 124 ]; then st=FAIL; note="(timeout ${TMO}s)"; fail=$((fail+1))
    elif r=$(env_reason "$log"); then st=SKIP; note="$r"; skip=$((skip+1))
    else
      st=FAIL; fail=$((fail+1))
      if grep -aqE 'error:|undefined reference|fatal error|ld returned|clang\+\+: error|g\+\+: error|build FAILED' "$log" && ! grep -aqE '^(PASS|ok)\b' "$log"; then note="(build) $(grep -aE 'error:|undefined reference|build FAILED' "$log" | head -1 | cut -c1-90)"
      else note="$(tail -n 1 "$log" | cut -c1-100)"; fi
    fi
    if grep -aq 'WITHOUT sanitizers' "$log"; then note="${note:+$note; }NO SANITIZERS (host_san: runtime missing)"; fi
    if [ "$grp" = python ] || [ "$id" = c18x-chain-extras ]; then
      rn=$(grep -aoE '^Ran [0-9]+ tests?' "$log" | tail -1); sk=$(grep -aoE 'skipped=[0-9]+' "$log" | tail -1)
      [ -n "$rn" ] && note="${note:+$note; }$rn${sk:+, $sk}"
    fi
    [ $st = PASS ] && [ -z "$(grep -ac . "$log" | grep -v '^0$')" ] && :
    [ $st = PASS ] && rm -f "$log"
  fi
  printf '%s\t%s\t%s\t%s\t%s\n' "$id" "$st" "$dt" "$grp" "$note" >> "$OUTD/results.tsv"
  printf '[%d/%d] %-4s %-46s %5ss %s\n' "$n" "$total" "$st" "$id" "$dt" "$note"
done <<< "$rows"
dtot=$(( $(date +%s) - T00 ))

# ---------------------------------------------------------------- the table
echo
echo "================================ host test summary ================================"
printf '%-46s %-14s %6s  %s\n' SUITE RESULT SECONDS NOTE
awk -F'\t' '{ r=$2; if (r=="SKIP") r="SKIP"; printf "%-46s %-14s %6s  %s\n", $1, r, $3, $5 }' "$OUTD/results.tsv"
echo "-----------------------------------------------------------------------------------"
echo "SKIPPED (not failures; this host cannot run them):"
awk -F'\t' '$2=="SKIP" { c[$5]++ } END { for (k in c) printf "  %4d  %s\n", c[k], k }' "$OUTD/results.tsv" | sort -rn
if [ $fail -gt 0 ]; then echo "FAILED:"; awk -F'\t' '$2=="FAIL" { printf "  %s  %s\n", $1, $5 }' "$OUTD/results.tsv"; fi
echo "== $total suites: $pass PASS, $fail FAIL, $skip SKIP; $((dtot/60)) min $((dtot%60)) s   (results: $OUTD/results.tsv; logs of non-passing suites: $OUTD/*.log)"
if [ $WRITEQ = 1 ]; then
  { echo "# tools/host_tests_quick.txt: the suites of 'run_all_host_tests.sh --quick'. Regenerate: run_all_host_tests.sh --write-quick (after a full pass)."
    echo "# PASS suites of at most ${QUICK_MAX_S:-8} s, taken in catalogue order until ${QUICK_BUDGET_S:-420} s are used."
    awk -F'\t' -v mx="${QUICK_MAX_S:-8}" -v bud="${QUICK_BUDGET_S:-420}" '$2=="PASS" && $3+0 <= mx && used + $3 + 1 <= bud { print $1; used += $3 + 1 }' "$OUTD/results.tsv"
  } > "$QUICK_FILE"
  echo "wrote $QUICK_FILE ($(grep -vc '^#' "$QUICK_FILE") suites)"
fi
[ $fail -eq 0 ] && [ $pass -gt 0 ]
