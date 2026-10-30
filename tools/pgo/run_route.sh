#!/usr/bin/env bash
# Run one route of the instrumented armhf game under qemu-arm and leave its raw profile in <run-dir>/route.profraw.
#
#   tools/pgo/run_route.sh <instrumented-game> <cooked-data-dir> <route.txt> <run-dir> <game-seconds>
#
# Environment: QEMU_ARM (default qemu-arm), SYSROOT (default /usr/arm-linux-gnueabihf), EXTRA_ENV="A=1 B=2" for more
# game switches, TIMEOUT (seconds, default 14400).  The cooked game data is read in place and never copied.
# The game runs on its virtual clock, so the profile does not depend on how fast qemu is.  <run-dir> must not exist.
set -uo pipefail
[ $# -eq 5 ] || { sed -n '2,9p' "$0"; exit 2; }
game=$1; cooked=$2; route=$3; run=$4; secs=$5
Q=${QEMU_ARM:-qemu-arm}; SYSROOT=${SYSROOT:-/usr/arm-linux-gnueabihf}
[ -x "$game" ] && [ -f "$cooked/sys/fst.bin" ] && [ -f "$route" ] || { echo "missing game, cooked data or route"; exit 2; }
mkdir "$run" || { echo "run dir exists"; exit 2; }
mkdir "$run/saves"
script_env=()
while IFS= read -r l; do case $l in '#!env '*) script_env+=("${l#'#!env '}");; esac; done < "$route"
inputline=$(sed -e 's/#.*$//' -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//' "$route" | grep -v '^$' | sed -e 's/,*$//' -e 's/^,*//' | paste -sd, - | tr -d '\n')
env_args=(PETARI_VCLOCK=1 PETARI_GAME_CLOCK=1 PETARI_QEMU_MARK=1 PETARI_NATIVE_ADDRESSES=1 PETARI_SHOT_MS=999999999
  SDL_AUDIODRIVER=dummy PETARI_THP_WORKERS=2 PETARI_VCLOCK_FE=1 PETARI_INPUT="$inputline"
  LLVM_PROFILE_FILE="$run/route.profraw")
env_args+=("${script_env[@]}")
# shellcheck disable=SC2206
env_args+=(${EXTRA_ENV:-})
start=$(date +%s)
nice -n 10 timeout --kill-after=20 "${TIMEOUT:-14400}" env "${env_args[@]}" "$Q" -cpu cortex-a9 -L "$SYSROOT" \
  "$game" "$cooked" "$run" "$secs" > "$run/console.log" 2>&1
rc=$?
echo "exit: $rc  wall: $(( $(date +%s) - start )) s  raw profile: $(stat -c %s "$run/route.profraw" 2>/dev/null || echo none) bytes"
exit $rc
