#!/usr/bin/env bash
# stackgate31: host tests of tools/check_display_stack.py (the display-queue callback thread's 4 KiB stack gate). No game build, no console, no network.
#   1 the fixture of the current tree (tests/stackgate31/display_ok.su, the .su lines of the listed functions from the Vita compile) passes; the worst chain has margin
#   2 mutants of the fixture turn it red: a 2 KiB buffer added back to PaceDisplay (336 -> 2384), a dynamic frame, logToFile's own 1100-byte line back on the callback's path,
#     the sum of small growths over the chain budget, a function missing from the .su (INCONCLUSIVE, exit 2, never a pass), no input (exit 2)
#   3 a real compiler's .su: a tiny unit with PaceDisplay / SyncDisplay (host clang -fstack-usage) is read (mangled names, anonymous namespace): clean passes, a char[2048] in PaceDisplay fails
#   4 tools/release_gates.py stack hands the same arguments to the gate
set -uo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
tmp=$(mktemp -d "${TMPDIR:-/tmp}/stackgate31.XXXXXX"); [[ -n ${KEEP:-} ]] || trap 'rm -rf "$tmp"' EXIT
fails=0; passes=0
ok() { echo "ok:   $*"; passes=$((passes + 1)); }
bad() { echo "FAIL: $*"; fails=$((fails + 1)); }
rc_is() { if [[ $3 == "$2" ]]; then ok "$1 (exit $3)"; else bad "$1: exit $3, wanted $2"; fi; }
has() { if grep -qF -- "$3" "$2"; then ok "$1"; else bad "$1: '$3' not in $2"; tail -8 "$2"; fi; }
gate="$root/tools/check_display_stack.py"
fx="$root/tests/stackgate31/display_ok.su"

# ---- 1. the current tree ---------------------------------------------------------------------------------------------------------------
mkdir -p "$tmp/ok"; cp "$fx" "$tmp/ok/a.su"
python3 -I "$gate" --su-dir "$tmp/ok" > "$tmp/o1.txt" 2>&1; rc_is "fixture passes" 0 $?
has "says ok and the margin" "$tmp/o1.txt" "stack ok: worst display-thread chain"
has "names the worst chain" "$tmp/o1.txt" "worst chain:"

# ---- 2. mutants -------------------------------------------------------------------------------------------------------------------------
mut() { # dir sed-expression
  mkdir -p "$tmp/$1"; sed -E "$2" "$fx" > "$tmp/$1/a.su"
}
mut m-pace '/PaceDisplayEPv/ s/\t336\t/\t2384\t/'          # a 2 KiB buffer back in PaceDisplay (q30cush31's tail + pose + line)
python3 -I "$gate" --su-dir "$tmp/m-pace" > "$tmp/o2.txt" 2>&1; rc_is "2 KiB buffer in PaceDisplay is red" 1 $?
has "names PaceDisplay" "$tmp/o2.txt" "PaceDisplay: frame 2384 bytes > budget 448"
has "names the 4 KiB breach" "$tmp/o2.txt" "bytes > the 4096-byte stack"
python3 -I "$gate" --su-dir "$tmp/m-pace" --hard-only > "$tmp/o2h.txt" 2>&1; rc_is "the hard limit alone is red too" 1 $?

mut m-sync '/SyncDisplayEPv\t/ s/\t616\t/\t2664\t/'
python3 -I "$gate" --su-dir "$tmp/m-sync" > "$tmp/o3.txt" 2>&1; rc_is "2 KiB buffer in SyncDisplay is red" 1 $?
mut m-even '/EvenShownE/ s/\t648\t/\t2696\t/'
python3 -I "$gate" --su-dir "$tmp/m-even" > "$tmp/o3b.txt" 2>&1; rc_is "2 KiB buffer in EvenShown is red" 1 $?
mut m-dyn '/PaceGateE/ s/\t144\tstatic/\t144\tdynamic,bounded/'
python3 -I "$gate" --su-dir "$tmp/m-dyn" > "$tmp/o4.txt" 2>&1; rc_is "a dynamic frame is red" 1 $?
has "dynamic named" "$tmp/o4.txt" "DYNAMIC frame"
# logToFile's own 1100-byte line back on the callback's path (what os_alarm.cpp had before stackgate31)
mut m-log '/logToFileBufP/ s/\t216\t/\t1304\t/'
python3 -I "$gate" --su-dir "$tmp/m-log" > "$tmp/o5.txt" 2>&1; rc_is "the log line buffer back on the stack is red" 1 $?
# each frame within its own budget, but the chain over 2560: Sync 768 + EvenShown 768 + 24 + 208 = 1768 is fine; push the formatters up to their budgets together
mut m-sum '/SyncDisplayEPv\t/ s/\t616\t/\t768\t/; /EvenShownE/ s/\t648\t/\t768\t/; /logToFileBufP/ s/\t216\t/\t320\t/; /port_log_summary_cb/ s/\t24\t/\t64\t/; /SmoothShownE/ s/\t184\t/\t256\t/'
python3 -I "$gate" --su-dir "$tmp/m-sum" > "$tmp/o6.txt" 2>&1; rc_is "all frames at their budgets still fits (sum 1920)" 0 $?
python3 -I "$gate" --su-dir "$tmp/m-sum" --our-budget 1800 > "$tmp/o6b.txt" 2>&1; rc_is "a chain over the summed budget is red" 1 $?
has "sum named" "$tmp/o6b.txt" "our frames sum to"
mkdir -p "$tmp/m-miss"; grep -v 'PaceGate' "$fx" > "$tmp/m-miss/a.su"
python3 -I "$gate" --su-dir "$tmp/m-miss" > "$tmp/o7.txt" 2>&1; rc_is "a listed function missing is INCONCLUSIVE, not a pass" 2 $?
has "missing named" "$tmp/o7.txt" "PaceGate: no frame in the .su files"
mkdir -p "$tmp/empty"; python3 -I "$gate" --su-dir "$tmp/empty" > "$tmp/o8.txt" 2>&1; rc_is "no .su files is INCONCLUSIVE" 2 $?
python3 -I "$gate" > "$tmp/o9.txt" 2>&1; rc_is "no input is INCONCLUSIVE" 2 $?
# the code before stackgate31 (port_log_summary / logToFile as the callback's log path, 1304 bytes) is matched by the fallback names
mkdir -p "$tmp/m-old"; grep -v 'logToFileBuf\|port_log_summary_cb' "$fx" > "$tmp/m-old/a.su"
printf 'os_alarm.cpp:750:port_log_summary\t24\tstatic\nos_alarm.cpp:587:_ZL9logToFilePKcSt9__va_list\t1304\tstatic\n' >> "$tmp/m-old/a.su"
python3 -I "$gate" --su-dir "$tmp/m-old" > "$tmp/o10.txt" 2>&1; rc_is "the pre-fix log path (1304-byte logToFile) is red" 1 $?
python3 -I "$gate" --su-dir "$tmp/m-old" --hard-only > "$tmp/o10h.txt" 2>&1; rc_is "...and within the hard 4 KiB (152 bytes to spare: the 192 of 61e121b2 less the 40-byte logPeriodicDropped the gate now counts)" 0 $?
has "the margin" "$tmp/o10h.txt" "margin 152"

# ---- 3. a real compiler's output --------------------------------------------------------------------------------------------------------
cxx=${CLANGXX:-clang++}
if command -v "$cxx" >/dev/null 2>&1 && command -v c++filt >/dev/null 2>&1; then
  mk() { # file bufsize
    cat > "$1" <<E
#include <cstdio>
extern "C" void port_log_summary_cb(const char*, ...);
namespace {
__attribute__((noinline)) void EvenShown(unsigned a, unsigned b, int c, int d) { volatile int x = a + b + c + d; (void)x; }
__attribute__((noinline)) void PaceDisplay(void*) {
    char buf[$2 + 1]; buf[0] = 0;
    std::snprintf(buf, sizeof buf, "%u", 1u);
    std::puts(buf);
    EvenShown(1, 2, 3, 4);
}
__attribute__((noinline)) void SyncDisplay(void*) { EvenShown(1, 2, 3, 4); }
}
void (*keep1)(void*) = PaceDisplay;
void (*keep2)(void*) = SyncDisplay;
E
  }
  mk "$tmp/real_small.cpp" 8; mk "$tmp/real_big.cpp" 2048
  mkdir -p "$tmp/r-small" "$tmp/r-big"
  ( cd "$tmp/r-small" && "$cxx" -O1 -fstack-usage -c "$tmp/real_small.cpp" -o r.o ) && ( cd "$tmp/r-big" && "$cxx" -O1 -fstack-usage -c "$tmp/real_big.cpp" -o r.o )
  if [[ -f $tmp/r-small/r.su && -f $tmp/r-big/r.su ]]; then
    # only these three functions exist in the probe: the gate reads them (the others are 'missing': INCONCLUSIVE 2), so the probe adds the rest from the fixture
    grep -v 'PaceDisplayEPv\|SyncDisplayEPv\|EvenShownEjjii' "$fx" > "$tmp/rest.su"
    cp "$tmp/rest.su" "$tmp/r-small/rest.su"; cp "$tmp/rest.su" "$tmp/r-big/rest.su"
    python3 -I "$gate" --su-dir "$tmp/r-small" > "$tmp/o11.txt" 2>&1; rc_is "real clang .su, small PaceDisplay: not red for PaceDisplay" 0 $?
    python3 -I "$gate" --su-dir "$tmp/r-big" > "$tmp/o12.txt" 2>&1; rc_is "real clang .su, char[2049] in PaceDisplay: red" 1 $?
    has "real mutant named" "$tmp/o12.txt" "PaceDisplay: frame"
  else
    bad "the host compiler wrote no .su file"
  fi
else
  echo "skip: no host clang++ / c++filt (the real-compiler check)"
fi

# ---- 4. release_gates.py stack ----------------------------------------------------------------------------------------------------------
python3 -I "$root/tools/release_gates.py" stack --su-dir "$tmp/ok" > "$tmp/o13.txt" 2>&1; rc_is "release_gates.py stack passes the fixture" 0 $?
python3 -I "$root/tools/release_gates.py" stack --su-dir "$tmp/m-pace" > "$tmp/o14.txt" 2>&1; rc_is "release_gates.py stack fails the mutant" 1 $?
has "release_gates doc lists stack" "$root/tools/release_gates.py" "  stack    --root"
has "checklist lists the stack gate" "$root/docs/RELEASE-3.0-CHECKLIST.md" "release_gates.py stack"

# ---- 5. fable31fix: the callee guard (tools/check_display_callees.py) and the maintenance notes ---------------------------------------------
src="$root/platform/vgx/vita/vgx_host_galaxy_vita.cpp"
python3 -I "$root/tools/check_display_callees.py" --root "$root" --strict > "$tmp/o15.txt" 2>&1; rc_is "every call in the display callbacks is in the tables" 0 $?
# mutant: a new call inside PaceDisplay's body (the first line after its opening brace)
python3 - "$src" "$tmp/mut.cpp" <<'PY'
import re, sys
t = open(sys.argv[1], errors="replace").read()
m = re.search(r"^void PaceDisplay\(void\*\) \{\n", t, re.M)
open(sys.argv[2], "w").write(t[:m.end()] + "    BrandNewHeavyThing(1);   // a call the gate does not know\n" + t[m.end():])
PY
python3 -I "$root/tools/check_display_callees.py" --file "$tmp/mut.cpp" --strict > "$tmp/o16.txt" 2>&1; rc_is "a new unlisted call is reported (strict: red)" 1 $?
has "names the new callee" "$tmp/o16.txt" "BrandNewHeavyThing"
python3 -I "$root/tools/check_display_callees.py" --file "$tmp/mut.cpp" > "$tmp/o17.txt" 2>&1; rc_is "without --strict it only lists (exit 0)" 0 $?
printf 'void Other() {}\n' > "$tmp/none.cpp"
python3 -I "$root/tools/check_display_callees.py" --file "$tmp/none.cpp" > "$tmp/o18.txt" 2>&1; rc_is "a renamed callback function is INCONCLUSIVE (exit 2)" 2 $?
has "the help carries the maintenance note" "$root/tools/check_display_stack.py" "any new function called from the display callbacks"
has "the checklist carries the maintenance note" "$root/docs/RELEASE-3.0-CHECKLIST.md" "must be added to \`FUNCS\` and \`CHAINS\`"

echo "stackgate31: $passes passed, $fails failed"
[[ $fails -eq 0 ]]
