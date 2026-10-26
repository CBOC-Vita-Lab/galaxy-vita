#!/usr/bin/env bash
# Host execution of the actual Vita timer adapter; test stubs do not validate SDK ABI or real GPU timing.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
cxx=${CXX:-clang++}
for diagnostics in 0 1; do
  "$cxx" -std=c++17 -O1 -g -Wall -Wextra -D__VGX_VITA__ -DVGX_NO_NEON \
    -DPORT_RUNTIME_DIAGNOSTICS="$diagnostics" -I"$root/tests/autoq_clock_stubs" \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    "$root/tests/vgx_autoq_clock.cpp" -o "$out/autoq-clock-$diagnostics"
  for target in unset 0 15 30; do
    expected=0
    if [[ $target == 30 ]]; then expected=30; fi
    for mode in ${AUTOQ_CLOCK_CASES:-coverage recovery hint hint-play ring trace}; do
      log="$out/$diagnostics-$target-$mode.txt"
      status=0
      if [[ $target == unset ]]; then
        env -u GXV_AUTOQ_TARGET GXV_AUTOQ_TIMERS=1 UBSAN_OPTIONS=halt_on_error=1 \
          "$out/autoq-clock-$diagnostics" "$mode" "$expected" >"$log" 2>&1 || status=$?
      else
        GXV_AUTOQ_TARGET="$target" GXV_AUTOQ_TIMERS=1 UBSAN_OPTIONS=halt_on_error=1 \
          "$out/autoq-clock-$diagnostics" "$mode" "$expected" >"$log" 2>&1 || status=$?
      fi
      if [[ $status != 0 ]]; then
        cat "$log"
        exit "$status"
      fi
      tail -n 1 "$log"
      if [[ $mode == trace && $target != 30 && -n ${AUTOQ_CLOCK_BASELINE_DIR:-} ]]; then
        cmp "$AUTOQ_CLOCK_BASELINE_DIR/$diagnostics-$target-trace.txt" "$log"
        printf 'PASS: identical legacy trace diagnostics=%s target=%s\n' "$diagnostics" "$target"
      fi
    done
  done
done
