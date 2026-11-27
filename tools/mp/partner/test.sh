#!/bin/bash
# Host test of the practice partner: partners against each other on 127.0.0.1, built with ASan + UBSan (g++: this machine's clang has
# no sanitizer runtime). Checks join, character pick, state flow with 20% loss, phrase, emote, pointer, bubble and rescue, leave,
# Same Wi-Fi discovery, a vanished peer, the version-mismatch words.
#   tools/mp/partner/test.sh          build and run everything (about 2 minutes)
#   tools/mp/partner/test.sh quick    skip the vanished-peer run (it waits out the 15 s timeout)
#   BREAK=1 tools/mp/partner/test.sh  deliberately breaks the first run (a wrong data hash): the test must FAIL
set -u
. "$(cd "$(dirname "$0")" && pwd)/../../../tests/mp/tmproot.sh"
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
out=${PARTNER_TEST_OUT:-$MP_TMP/partner_tests}
mkdir -p "$out"
cxx=${CXX:-g++}
N=$root/net/src; M=$root/platform/src/mp; P=$here
SAN="-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer"
if [ ! -x "$out/partner" ] || [ -n "$(find "$P" "$M" "$N" -newer "$out/partner" \( -name '*.cpp' -o -name '*.h' -o -name '*.hpp' \) 2>/dev/null | head -1)" ]; then
  echo "building with $cxx ($SAN) ..."
  gcc -O1 -c -w $SAN "$N/online/third_party/monocypher/monocypher.c" -o "$out/monocypher.o" || exit 2
  nice -n 10 $cxx -std=c++20 -g -O1 $SAN -pthread -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wno-unused-function -Wno-class-memaccess \
    -I"$root/platform/include" -I"$root/net/include" -I"$M" -I"$N" -I"$N/online" -o "$out/partner" \
    "$P/partner.cpp" "$P/partner_udp.cpp" "$P/partner_stubs.cpp" \
    "$M/mp_core.cpp" "$M/mp_core_sys.cpp" "$M/mp_core_vita.cpp" "$M/mp_reliable.cpp" "$M/mp_lan.cpp" "$M/mp_snapshot.cpp" "$M/mp_config.cpp" "$M/mp_log.cpp" \
    "$M/mp_transport_loop.cpp" "$M/mp_transport_vlink.cpp" "$M/mp_conn_online.cpp" "$M/mp_conn_direct.cpp"  \
    "$M/mp_demo.cpp" "$M/mp_keepsafe.cpp" "$M/mp_savefile.cpp" "$M/mp_savecheck.cpp" \
    "$N/vlink.cpp" "$N/vlink_udp.cpp" "$N/online/np_net.cpp" "$N/online/np_stun.cpp" "$N/online/np_crypto.cpp" "$N/online/np_fault.cpp" \
    "$out/monocypher.o" || exit 2
fi
PARTNER="$out/partner"
pass=0; fail=0
ok()   { pass=$((pass + 1)); echo "  ok    $1"; }
bad()  { fail=$((fail + 1)); echo "  FAIL  $1"; }
has()  { if grep -Eq "$2" "$1" 2>/dev/null; then ok "$3"; else bad "$3   (no /$2/ in $1)"; fi; }
hasnt() { if grep -Eq "$2" "$1" 2>/dev/null; then bad "$3   (found /$2/ in $1)"; else ok "$3"; fi; }
num()  { sed -n "s/.*$2.*/\1/p" "$1" 2>/dev/null | tail -1; }
atleast() {   # file, regex with one group, minimum, description
  local v; v=$(sed -En "s/^.*$2/\1/p" "$1" 2>/dev/null | tail -1)
  if [ -n "$v" ] && [ "$v" -ge "$3" ]; then ok "$4 ($v)"; else bad "$4 (got '${v:-none}', want >= $3)"; fi
}
clean() {     # no sanitizer report in the run
  hasnt "$1.log" 'AddressSanitizer|runtime error:|LeakSanitizer' "$2: sanitizers silent"
  hasnt "$1.out" 'AddressSanitizer|runtime error:|LeakSanitizer' "$2: sanitizers silent (terminal)"
}
cd "$out" || exit 2
rm -f *.log *.out
export ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 UBSAN_OPTIONS=print_stacktrace=1

echo "== 0. the README names the protocol this tree speaks"
want=$("$PARTNER" --version 2>&1 | sed -En 's/^partner: protocol ([0-9]+),.*/\1/p' | head -1)
doc=$(sed -En 's/.*`partner: protocol ([0-9]+),.*/\1/p' "$P/README.md" | head -1)
if [ -n "$want" ] && [ "$want" = "$doc" ]; then ok "README: protocol $doc = the build's"; else bad "README says protocol '${doc:-none}', the build speaks '${want:-?}'"; fi

echo "== 1. by address, 20% loss, phrase / emote / pointer / bubble + rescue / leave"
jhash=00000000; [ "${BREAK:-0}" = 1 ] && jhash=12345678
$PARTNER --host --udp-port 47110 --name HostP --look mario --wander --anchor 0,0,0 --chat-every 3 --pointer-every 4 --duration 30 --log s1h.log --quiet > s1h.out 2>&1 &
hp=$!
sleep 1.5
timeout 60 $PARTNER --join 127.0.0.1:47110 --name JoinP --look mario --follow --loss 20 --delay 30 --jitter 20 --chat-every 3 --pointer-every 4 \
  --bubble-me 4 --leave-after 20 --once --data-hash $jhash --log s1j.log --quiet > s1j.out 2>&1
jrc=$?
wait $hp; hrc=$?
[ $jrc = 0 ] && ok "joiner exit 0" || bad "joiner exit $jrc"
[ $hrc = 0 ] || [ $hrc = 1 ] && ok "host exit $hrc" || bad "host exit $hrc"
has s1h.log 'joined slot=0 look=0 wished=0' "host: Mario as wished"
has s1j.log 'joined slot=1 look=[1-5] wished=0' "joiner: Mario was taken, the host gave another character"
has s1j.out 'wished for was taken' "joiner says so"
atleast s1j.log 'totals: sent [0-9]+ states, heard ([0-9]+) from the leader.*' 150 "joiner heard the host's states"
atleast s1h.log 'totals: sent [0-9]+ states, heard ([0-9]+) from the leader.*' 150 "host heard the joiner's states"
has s1j.log 'fault injection: [0-9]+ sent, [1-9][0-9]* dropped' "joiner's own sends were dropped (--loss 20)"
atleast s1j.log 'totals: .* ([0-9]+) poses dropped by --loss.*' 5 "joiner dropped poses it heard (--loss 20)"
has s1h.out 'Slot 1 \(JoinP\) said phrase' "host got the joiner's phrase"
has s1j.out 'Slot 0 \(HostP\) said phrase' "joiner got the host's phrase"
has s1h.out 'Slot 1 \(JoinP\) sent emote' "host got the joiner's emote"
has s1j.out 'Slot 0 \(HostP\) sent emote' "joiner got the host's emote"
has s1h.out 'Slot 1 \(JoinP\) is using the Star Pointer' "host saw the joiner's pointer"
has s1j.out 'Slot 0 \(HostP\) is using the Star Pointer' "joiner saw the host's pointer"
has s1j.out 'Fell into a bubble' "joiner fell into a bubble"
has s1h.out 'Slot 1 \(JoinP\) is in a bubble' "host got BUBBLE on"
has s1h.out 'going over to pop it' "host went to rescue"
has s1j.out 'Popped by slot 0' "bubble popped by the host's touch"
has s1h.out 'Slot 1 \(JoinP\) left its bubble' "host got BUBBLE off"
has s1h.log '\[mp\] slot 1 left reason=leave' "host saw the polite leave"
has s1j.log 'phase idle error=0' "joiner ended cleanly"
clean s1h "host"; clean s1j "joiner"

echo "== 2. Same Wi-Fi discovery (two ports on one computer)"
$PARTNER --host --udp-port 47100 --bcast 127.0.0.1:47101 --name HostL --look toad --wander --anchor 0,0,0 --duration 14 --log s2h.log --quiet > s2h.out 2>&1 &
hp=$!
sleep 1
timeout 40 $PARTNER --join-lan --udp-port 47101 --bcast 127.0.0.1:47100 --name JoinL --look toad --follow --scan-for 20 --leave-after 6 --once --log s2j.log --quiet > s2j.out 2>&1
jrc=$?
wait $hp
[ $jrc = 0 ] && ok "joiner exit 0" || bad "joiner exit $jrc"
has s2j.out 'Found room "HostL"' "joiner found the room by its beacon"
has s2j.out 'In the room as slot 1, character' "joiner joined it"
has s2h.log 'joined slot=0 look=2 wished=2' "host: Toad as wished"
atleast s2j.log 'totals: sent [0-9]+ states, heard ([0-9]+) from the leader.*' 50 "joiner follows the host over the LAN link"
has s2j.log 'broadcast targets: 127.0.0.1:47100' "log names the broadcast targets"
clean s2h "host"; clean s2j "joiner"

echo "== 3. version words"
$PARTNER --host --udp-port 47130 --bcast 127.0.0.1:47131 --name HostV --data-hash 11111111 --duration 12 --log s3h.log --quiet > s3h.out 2>&1 &
hp=$!
sleep 1
timeout 30 $PARTNER --join-lan --udp-port 47131 --bcast 127.0.0.1:47130 --name JoinV --data-hash 22222222 --scan-for 6 --once --log s3j.log --quiet > s3j.out 2>&1
jrc=$?
[ $jrc = 1 ] && ok "LAN joiner gives up with exit 1" || bad "LAN joiner exit $jrc (want 1)"
has s3j.out 'Version mismatch: same protocol \([0-9]+\) but the Vita.s fingerprint is [0-9a-f]{8} and this partner speaks protocol [0-9]+, build id "[^"]*", data hash 22222222' "LAN: says what differs (protocol, build id, data hash)"
timeout 30 $PARTNER --join 127.0.0.1:47130 --name JoinV2 --data-hash 22222222 --once --log s3j2.log --quiet > s3j2.out 2>&1
jrc=$?
[ $jrc = 3 ] && ok "by address: exit 3 (version)" || bad "by address exit $jrc (want 3)"
has s3j2.out 'Version mismatch: the host refused us as a different version; this partner speaks protocol [0-9]+' "by address: says exactly that"
wait $hp
clean s3h "host"; clean s3j "joiner"; clean s3j2 "joiner (address)"

if [ "${1:-}" != quick ]; then
echo "== 4. a peer vanishes without BYE (host sees the timeout)"
$PARTNER --host --udp-port 47120 --name HostD --wander --anchor 0,0,0 --duration 27 --log s4h.log --quiet > s4h.out 2>&1 &
hp=$!
sleep 1
timeout 40 $PARTNER --join 127.0.0.1:47120 --name JoinD --follow --drop-after 4 --log s4j.log --quiet > s4j.out 2>&1
jrc=$?
[ $jrc = 0 ] && ok "dropping partner exits 0" || bad "dropping exit $jrc"
has s4j.out 'Dropping off the network now without saying goodbye' "it says what it does"
wait $hp
has s4h.log '\[mp\] slot 1 left reason=(timeout|lost)' "host timed the vanished peer out"
hasnt s4h.log 'reason=leave' "no goodbye was sent"
clean s4h "host"
fi

echo
echo "$pass passed, $fail failed (logs in $out)"
[ $fail = 0 ]
