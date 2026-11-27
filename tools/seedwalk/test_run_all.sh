#!/bin/bash
# test_run_all.sh - the dry run of the whole harvest chain on fixtures (no emulator): plan.py make -> run_all.sh (leg.sh in a network namespace, with a
# FAKE run.sh that writes what the game writes) -> plan.py resume -> collect.py; and run_all.sh's guards: a build running before and during a leg, low
# disk, the STOP file, a second run (finished legs skipped), one follow-up leg per incomplete leg. Needs root (unshare -n), as run_all.sh does.
#   bash tools/seedwalk/test_run_all.sh          exit 0 = every check passed
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
T=$(mktemp -d "${TMPDIR:-/tmp}/sw-runall.XXXXXX")
trap 'pkill -f "sw-fake-build31.sh" 2>/dev/null; [ "${KEEP:-0}" = 1 ] || rm -rf "$T"' EXIT
PASS=0; FAIL=0
ok() { if eval "$1"; then PASS=$((PASS + 1)); else FAIL=$((FAIL + 1)); echo "FAIL: $2"; fi; }

# a fake emulator setup: run.sh writes the run folder and the card as the game would, for the stops of the env file's PETARI_FLOW
V=$T/v3k; mkdir -p "$V/pkgsc/shadercache/b4d4c2a9/v3" "$V/runs" "$V/pref/ux0/data/smg" "$V/inputs"
echo seed > "$V/pkgsc/shadercache/vgx_shaderlist.bin"
cat > "$V/run.sh" <<'EOF'
#!/bin/bash
V=$(cd "$(dirname "$0")" && pwd)
EBOOT=$1; ENVF=$2
exec 9>"$V/run.lock"; flock -w 60 9 || exit 3
OUT=$V/runs/$TAG; DATA=$V/pref/ux0/data/smg
rm -rf "$OUT"; mkdir -p "$OUT"
[ "${RESETCACHE:-0}" = 1 ] && rm -rf "$DATA/shadercache"
python3 -I -c 'import socket; print([x for _, x in socket.if_nameindex()])' >> "$V/netns.txt"
echo "$(nice)" >> "$V/nice.txt"
env | cut -d= -f1 >> "$V/env_seen.txt"
cp "$ENVF" "$OUT/env.txt"
FLOW=$(sed -n 's/^PETARI_FLOW=//p' "$ENVF")
IFS=, read -r -a ST <<< "$FLOW"
N=${#ST[@]}
L=$DATA/log.txt; : > "$L"
echo "00:00:01.000 profile: word=0123456789abcdef gen=2 switches=30" >> "$L"
sleep "$(cat "$V/fake_sleep" 2>/dev/null || echo 1)"   # (run_all gives a leg a clean environment: the fake's knobs are files)
FAKE_DROP=$(cat "$V/fake_drop" 2>/dev/null)
i=0
for s in "${ST[@]}"; do
    i=$((i + 1)); g=${s%%:*}
    echo "00:00:1$i.000 cover: stage=$g why=left profile=0123456789abcdef play_s=90.0 links=1 link_ms=2 link_worst_ms=2 compiles=0 compile_ms=0 new_recs=1 new_candraw=0 uploads=0 targets=0 hold_ms=10" >> "$L"
    if [ -n "${FAKE_DROP:-}" ] && [ "$g" = "$FAKE_DROP" ] && [ ! -e "$V/dropped-once" ]; then touch "$V/dropped-once"; continue; fi
    echo "00:00:2$i.000 flow: entry $i of $N ($g) done: 5400 turns of play" >> "$L"
done
grep -q " done:" "$L" && [ "$(grep -c ' done:' "$L")" = "$N" ] && echo "00:00:59.000 flow: done: all $N entries" >> "$L"
mkdir -p "$DATA/shadercache/b4d4c2a9-sw31/v3/f/3A"
printf 'GXPfake1' > "$DATA/shadercache/b4d4c2a9-sw31/v3/f/3A/3A1B2C3D4E5F6071.gxp"
printf 'VGXSTW01' > "$DATA/shadercache/vgx_stagewarm.bin"
cp "$L" "$OUT/log.txt"; echo x > "$OUT/vita3k.log"
echo "STOPAT matched (+3 s) after 5 s wall" > "$OUT/status.txt"
EOF
chmod +x "$V/run.sh"
echo eboot > "$T/eboot.bin"
mkdir -p "$T/save/saves/title"   # (the PETARI_BOOT legs need a save: SAVE_TEMPLATE, as the real run is given one)
mk() { python3 -I "$HERE/plan.py" make --out "$1" --passes p60:1.5 --stops-per-leg 2 --no-menus --table "$HERE/fixtures/fake_table.txt" > /dev/null; }
run() { env V3K="$V" EBOOT="$T/eboot.bin" SEED="$V/pkgsc/shadercache" SAVE_TEMPLATE="$T/save" POLL=1 "$@"; }
legs_run() { [ -f "$1/runall/legs.tsv" ] && wc -l < "$1/runall/legs.tsv" || echo 0; }

# 1. the plain run: two legs, each in its namespace and niced, then collect
O=$T/o1; mk "$O"
run bash "$HERE/run_all.sh" "$O" > "$T/r1.txt" 2>&1; rc=$?
ok '[ $rc = 0 ]' "run_all exit $rc: $(tail -5 "$T/r1.txt")"
ok '[ "$(legs_run "$O")" = 2 ]' "two leg runs (legs.tsv: $(cat "$O/runall/legs.tsv" 2>/dev/null))"
ok '[ "$(ls "$O"/legs/*/DONE 2>/dev/null | wc -l)" = 2 ]' "both legs DONE"
ok '[ -s "$V/netns.txt" ] && ! grep -qv "^\[.lo.\]$" "$V/netns.txt"' "every leg saw only the loopback: $(sort -u "$V/netns.txt")"
ok 'grep -q "^10$" "$V/nice.txt"' "legs ran at nice 10: $(sort -u "$V/nice.txt" | tr '\n' ' ')"
ok 'grep -q "netns: interfaces \[.lo.\]" "$O"/runall/out/*.txt' "the leg log shows the namespace check"
ok 'grep -q "^state: finished" "$O/progress.txt"' "progress says finished: $(sed -n 3p "$O/progress.txt" 2>/dev/null)"
ok 'grep -q "collect.py: exit 0" "$O/runall/events.log"' "collect passed: $(grep collect "$O/runall/events.log" | tail -1)"
ok '[ -f "$O/harvest/MANIFEST.json" ]' "harvest written"
# 2. a second run: nothing runs again
run bash "$HERE/run_all.sh" "$O" > "$T/r2.txt" 2>&1; rc=$?
ok '[ $rc = 0 ] && [ "$(legs_run "$O")" = 2 ]' "second run ran nothing (rc $rc, $(legs_run "$O") leg runs)"

# 3. low disk: stops before any leg, exit 5
O=$T/o3; mk "$O"
run env MIN_FREE_GB=100000000 bash "$HERE/run_all.sh" "$O" > "$T/r3.txt" 2>&1; rc=$?
ok '[ $rc = 5 ] && [ "$(legs_run "$O")" = 0 ] && grep -q "stopped: low disk" "$O/progress.txt"' "low disk stops cleanly (rc $rc): $(sed -n 3p "$O/progress.txt")"
# 4. the STOP file
O=$T/o4; mk "$O"; touch "$O/STOP"
run bash "$HERE/run_all.sh" "$O" > "$T/r4.txt" 2>&1; rc=$?
ok '[ $rc = 5 ] && [ "$(legs_run "$O")" = 0 ]' "STOP file stops (rc $rc)"

# 5. a build before and during a leg: waits, stops the leg, runs it again after; the interrupted run is not a try
O=$T/o5; mk "$O"
bash -c 'exec -a sw-fake-build31.sh sleep 4' & BP=$!
echo 5 > "$V/fake_sleep"
run env BUSY_PATTERN='sw-fake-build31\.sh' bash "$HERE/run_all.sh" "$O" > "$T/r5.txt" 2>&1 &
RP=$!
sleep 7   # (the first build has ended; leg 1 runs now)
bash -c 'exec -a sw-fake-build31.sh sleep 4' & BP=$!
wait $RP; rc=$?; rm -f "$V/fake_sleep"
ok '[ $rc = 0 ]' "run with builds: rc $rc"
ok 'grep -q "waiting: a build is running" "$O/runall/events.log"' "waited for the build"
ok 'grep -q "interrupted: build" "$O/runall/events.log"' "a running leg was stopped by a build: $(grep -c interrupted "$O/runall/events.log")"
ok '[ "$(ls "$O"/legs/*/DONE | wc -l)" = 2 ]' "both legs DONE after the builds"
ok '[ "$(cat "$O"/runall/tries/* | sort -u | tr -d "\n")" = 1 ]' "an interrupted run is not a try: $(cat "$O"/runall/tries/*)"
ok '! pgrep -f "sw-fake-build31" >/dev/null' "no fake build left"

# 6. an incomplete leg: one follow-up for its missing stop, then complete; a second resume makes none
O=$T/o6; mk "$O"; rm -f "$V/dropped-once"
echo EggStarGalaxy > "$V/fake_drop"
run bash "$HERE/run_all.sh" "$O" > "$T/r6.txt" 2>&1; rc=$?; rm -f "$V/fake_drop"
ok '[ $rc = 0 ]' "incomplete + resume run: rc $rc"
ok 'grep -q "resume round 1: follow-up legs: .*-r1" "$O/runall/events.log"' "resume made a follow-up: $(grep resume "$O/runall/events.log" | head -2)"
fu=$(ls -d "$O"/legs/*-r1 2>/dev/null | head -1)
ok '[ -n "$fu" ] && [ -f "$fu/DONE" ] && grep -q "EggStarGalaxy:1" "$fu/spec.json"' "the follow-up walked the missing stop: $fu"
ok '[ "$(python3 -I "$HERE/plan.py" resume "$O" | tail -1)" = "follow-up legs: none" ]' "a second resume makes no second follow-up"
ok 'grep -q "resume round 2: follow-up legs: none" "$O/runall/events.log"' "round 2 found nothing"
ok 'grep -q "collect.py: exit 0" "$O/runall/events.log"' "collect accepts a leg whose follow-up finished its stops: $(grep "collect.py:" "$O/runall/events.log" | cut -c1-200)"

# 7. leg.sh refuses an output inside the repository; run_all refuses a missing plan
run bash "$HERE/run_all.sh" "$T/nothing" > "$T/r7.txt" 2>&1; rc=$?
ok '[ $rc = 2 ]' "no plan: rc $rc"
# 8. the environment a leg gets is clean (no proxy, no token)
ok '! grep -q -i "proxy\|token" "$V/env_seen.txt"' "a leg's environment is clean: $(grep -i -c "proxy\|token" "$V/env_seen.txt")"

echo "test_run_all: $PASS of $((PASS + FAIL)) checks passed"
[ $FAIL = 0 ]
