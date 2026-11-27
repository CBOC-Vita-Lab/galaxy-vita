#!/bin/bash
# netport31: each port's host test must FAIL when its fix is taken out. Copies net/ to a scratch folder per mutant, breaks one line there, runs
# the test that guards it (online_test --netport31, or the HTTPS test against its local server), and expects a failure.
#   net/tests/netport31_mutants.sh [scratch dir]      MBEDTLS=<host64 prefix> (the HTTPS mutants; without it they are skipped)
set -u
cd "$(dirname "$0")/.."
NET=$(pwd)
WORK=${1:-/tmp/np-netport31-mutants}
rm -rf "$WORK"; mkdir -p "$WORK"
caught=0; missed=0; skipped=0
mutant() {   # name, test (online|https), file, python-literal old, python-literal new
    local name=$1 test=$2 file=$3 old=$4 new=$5 d="$WORK/$1"
    if [ "$test" = https ] && [ -z "${MBEDTLS:-}" ]; then echo "SKIP   $name (MBEDTLS not set)"; skipped=$((skipped + 1)); return; fi
    mkdir -p "$d"; cp -r "$NET/src" "$NET/include" "$NET/tests" "$d/"
    if ! python3 -I - "$d/$file" "$old" "$new" <<'EOF'
import sys
p, old, new = sys.argv[1], sys.argv[2], sys.argv[3]
s = open(p).read()
if s.count(old) != 1: sys.exit("pattern found %d times" % s.count(old))
open(p, "w").write(s.replace(old, new))
EOF
    then echo "BROKEN $name (the line to change was not found)"; missed=$((missed + 1)); return; fi
    if [ "$test" = online ]; then bash "$d/tests/run_online_tests.sh" "$d/out" --netport31 > "$d/log" 2>&1
    else MBEDTLS=$MBEDTLS bash "$d/tests/run_https_test.sh" "$d/out" > "$d/log" 2>&1; fi
    if [ $? -ne 0 ] && grep -q "FAIL" "$d/log"; then echo "CAUGHT $name"; caught=$((caught + 1)); rm -rf "$d"
    else echo "MISSED $name (see $d/log)"; missed=$((missed + 1)); fi
}
mutant held-io-signal online src/online/np_signal_http.cpp 'j.waitMs ? HeldIoMs(j.waitMs) : 0, &err)' '0, &err)'
mutant held-io-plain online src/online/np_http_plain.cpp 'r->ioTimeoutMs = ioTimeoutMs ? ioTimeoutMs : DefaultIoMs();' 'r->ioTimeoutMs = DefaultIoMs();'
mutant give-way online src/online/np_signal_http.cpp ' || HttpOthersWaiting(cur_)' ''
mutant abandon-closes online src/online/np_http_plain.cpp '        if (__atomic_load_n(&r->state, __ATOMIC_ACQUIRE) == 2) return false;' ''
mutant held-io-https https src/online/np_http.cpp '        rq.io_timeout_ms = ioTimeoutMs;' ''
mutant waiting-self https src/np_https.cpp '&g_slots[i] != own && ' ''
mutant ctype-line https src/np_https.cpp '            if (strncasecmp(*h, "Content-Type:", 13) == 0) {' '            if (false) {'
mutant connect-close https src/np_https.cpp '    if (r != 0) { SockClose(&g_conn.sock); return Fail(c, NP_HTTPS_E_CONNECT' '    if (r != 0) { return Fail(c, NP_HTTPS_E_CONNECT'
# s1117b (Strikers 1117b): 8 request slots; a fire-and-forget request that cannot start is counted and logged
mutant slots-eight https src/np_https.h '#define NP_HTTPS_SLOTS 8' '#define NP_HTTPS_SLOTS 4'
mutant fire-drop-counted online src/online/np_http.cpp '        ++g_fireDropped;' ''
echo "netport31 mutants: $caught caught, $missed missed, $skipped skipped"
[ $missed -eq 0 ]
