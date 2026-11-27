#!/bin/bash
# leg.sh - run ONE seedwalk leg in the headless Vita3K of $V3K (retail profile, real shader compiler) and copy what it learned into the
# leg's own folder.   leg.sh OUT LEG_ID
#   OUT     the walk's output folder made by `plan.py make --out OUT` (OUT/plan.json, OUT/legs/<id>/{spec.json,env.txt,input.txt})
#   LEG_ID  a leg id of OUT/plan.json (plan.py list OUT)
# Environment:
#   EBOOT=<eboot.bin>      REQUIRED: the candidate build (its sha256 is recorded; a finished leg of another build is run again)
#   SEED=<shadercache dir> the candidate seed, installed as app0:shadercache (default $V3K/pkgsc/shadercache); its sha-of-listing is recorded
#   V3K=<folder>           REQUIRED: the emulator setup (its run.sh and README.txt); run.sh holds its run.lock: ONE emulator at a time, never beside a game build
#   FORCE=1                run again although legs/<id>/DONE matches      DRY=1   check everything, print the run.sh call, do not run the emulator
#   SAVE_TEMPLATE=<dir>    folder holding saves/ for PETARI_BOOT legs (default OUT/save-template, made by the menus leg's run)
# Resumable and cheap to re-run: a leg with a matching DONE is skipped in a second; an incomplete leg is re-run whole, or `plan.py resume OUT` makes a
# follow-up leg for only its missing stops. Each leg starts from the candidate seed and an EMPTY card cache (RESETCACHE=1, GXV_SHADER_CACHE_TAG in
# its env), so the card's tagged folder is exactly what the seed lacked.
# Writes: only under OUT/legs/<id>/ (log + status in run/, the card's delta in card/) and OUT/save-template/ once. The emulator's own working state
# ($V3K/pref/ux0/data/smg, $V3K/home) is reset by run.sh at every start, as always; its per-run folder $V3K/runs/<tag> is copied out and removed.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${1:-}; LEG=${2:-}
[ -n "$OUT" ] && [ -n "$LEG" ] || { sed -n 2,17p "$0"; exit 2; }
: "${V3K:?leg.sh: V3K=<the headless Vita3K setup folder, with run.sh> is required}"
V3K=$(readlink -m "$V3K")
: "${EBOOT:?leg.sh: EBOOT=<eboot.bin> is required}"
SEED=${SEED:-$V3K/pkgsc/shadercache}
PY="python3 -I"

# the output folder must be its own: not the game data, not the emulator's folders, not the repo
OUTR=$(readlink -m "$OUT")
case "$OUTR" in
    "$V3K"/in|"$V3K"/in/*|"$V3K"/pref|"$V3K"/pref/*|"$V3K"/app|"$V3K"/app/*|"$V3K"/home|"$V3K"/home/*|/|/home|"$(readlink -m "${HOME:-/}")") echo "leg.sh: $OUTR is not a folder for harvest output"; exit 2;;
esac
case "$OUTR" in "$(cd "$HERE/../.." && pwd)"/*) echo "leg.sh: $OUTR is inside the repository: harvest output (compiled shaders, logs) never goes there"; exit 2;; esac
[ -f "$OUTR/plan.json" ] || { echo "leg.sh: $OUTR/plan.json missing (plan.py make --out ...)"; exit 2; }
LD=$OUTR/legs/$LEG
[ -f "$LD/spec.json" ] || { echo "leg.sh: no leg $LEG in $OUTR"; exit 2; }
[ -f "$EBOOT" ] || { echo "leg.sh: no eboot at $EBOOT"; exit 2; }
[ -d "$SEED" ] || { echo "leg.sh: no candidate seed folder at $SEED"; exit 2; }

# the env allow-list, again, on the file that is about to be used (a hand edit cannot slip a text switch in)
$PY "$HERE/plan.py" check-env "$LD/env.txt" || { echo "leg.sh: $LD/env.txt is refused"; exit 3; }
eval "$($PY "$HERE/plan.py" leginfo "$OUTR" "$LEG")"

EBSHA=$(sha256sum "$EBOOT" | cut -d' ' -f1)
PLANFP=$($PY -c 'import json,sys;print(json.load(open(sys.argv[1]))["fingerprint"])' "$OUTR/plan.json")
SEEDID=$(cd "$SEED" && find . -type f | LC_ALL=C sort | xargs -r sha256sum | sha256sum | cut -c1-16)
if [ -f "$LD/DONE" ] && [ "${FORCE:-0}" != 1 ]; then
    if grep -q "\"ebsha\": \"$EBSHA\"" "$LD/run-info.json" 2>/dev/null && grep -q "\"seed\": \"$SEEDID\"" "$LD/run-info.json" 2>/dev/null; then
        echo "leg $LEG: done already for this build and seed (FORCE=1 to run again)"; exit 0
    fi
    echo "leg $LEG: DONE belongs to another build or seed: run again"
fi

# stops, input, save
INPUT=
case "$INPUTKIND" in
    input.txt) INPUT=$LD/input.txt;;
    menu-fast) INPUT=$V3K/inputs/menu-fast.txt;;
    '') ;;
    *) echo "leg.sh: unknown input kind $INPUTKIND"; exit 2;;
esac
[ -z "$INPUT" ] || [ -f "$INPUT" ] || { echo "leg.sh: no input file $INPUT"; exit 2; }
SAVE=${SAVE_TEMPLATE:-$OUTR/save-template}
FILES=
if [ "$BOOT" = 1 ]; then
    if [ -d "$SAVE/saves" ]; then FILES="$SAVE/saves"
    else echo "leg.sh: $LEG needs a save for PETARI_BOOT: $SAVE/saves is missing. Run the menus leg first (it makes the template), or put a saves/ folder there."; [ "${DRY:-0}" = 1 ] || exit 2; fi
fi

TAG=sw31-$PLANFP-$LEG
echo "leg $LEG ($KIND, $NSTOPS stops): eboot sha ${EBSHA:0:12}, seed $SEEDID, plan $PLANFP, wall cap $WALLCAP s"
CALL="TAG=$TAG SHOTEVERY=0 KEEPDATA=0 RESETCACHE=1 SHADERPKG=$SEED STOPAT='$STOPAT' STOPAFTER=$STOPAFTER WALLMAX=$WALLCAP ${FILES:+FILES=$FILES }$V3K/run.sh $EBOOT $LD/env.txt $WALLCAP ${INPUT}"
if [ "${DRY:-0}" = 1 ]; then echo "DRY: $CALL"; exit 0; fi

# a fresh leg folder (only the run's own outputs are cleared; spec/env/input stay)
rm -rf "$LD/run" "$LD/card" "$LD/DONE" "$LD/status.json" "$LD/run-info.json"
mkdir -p "$LD/run" "$LD/card"
T0=$(date +%s)
env TAG="$TAG" SHOTEVERY=0 KEEPDATA=0 RESETCACHE=1 SHADERPKG="$SEED" STOPAT="$STOPAT" STOPAFTER="$STOPAFTER" WALLMAX="$WALLCAP" ${FILES:+FILES="$FILES"} \
    "$V3K/run.sh" "$EBOOT" "$LD/env.txt" "$WALLCAP" $INPUT > "$LD/run/run.out" 2>&1
RC=$?
WALL=$(( $(date +%s) - T0 ))

# copy out: the run's own folder, then the card's delta (under the same lock run.sh uses, so no other run reuses the data folder meanwhile)
R=$V3K/runs/$TAG
for f in log.txt vita3k.log status.txt summary.txt env.txt input.txt; do [ -f "$R/$f" ] && cp "$R/$f" "$LD/run/$f"; done
DATA=$V3K/pref/ux0/data/smg
(
    flock -w 600 9 || { echo "leg.sh: could not take $V3K/run.lock to read the card"; exit 1; }
    [ -d "$DATA/shadercache" ] && cp -a "$DATA/shadercache/." "$LD/card/"
    if [ ! -d "$OUTR/save-template/saves" ] && [ -d "$DATA/saves" ] && [ "$KIND" = menus ]; then mkdir -p "$OUTR/save-template" && cp -a "$DATA/saves" "$OUTR/save-template/"; fi
) 9>"$V3K/run.lock"
[ -d "$R" ] && rm -rf "$R"
printf '{"ebsha": "%s", "seed": "%s", "plan": "%s", "rc": %d, "wall_s": %d, "tag": "%s"}\n' "$EBSHA" "$SEEDID" "$PLANFP" "$RC" "$WALL" "$TAG" > "$LD/run-info.json"

$PY "$HERE/plan.py" leg-status "$OUTR" "$LEG"
S=$?
echo "leg $LEG: run.sh rc $RC, $WALL s wall; log $(wc -c < "$LD/run/log.txt" 2>/dev/null || echo 0) bytes; card $(find "$LD/card" -name '*.gxp' 2>/dev/null | wc -l) compiled files"
exit $S
