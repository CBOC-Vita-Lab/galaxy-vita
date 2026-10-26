#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
sdk=${VITASDK:-/opt/vitasdk}
part=${1:-all}
if test "$part" = host-perf; then
  out=${2:-$(mktemp -d)}
  mkdir -p "$out"
  common=(-std=c++20 -O1 -g -D__vita__ -DPORT_LOGGING=1 -I"$root/platform/include"
    -fsanitize=address,undefined -fno-omit-frame-pointer)
  "${CXX:-clang++}" "${common[@]}" "$root/tests/vita_perf_audit.cpp" -pthread -o "$out/perf"
  for scenario in cpu-scope cpu-failure stale-player forced-count cheap-skips backlog turn-costs forced-costs fast-skip-costs double-done disabled-costs paced; do
    GXV_WORKLOAD_PROBE=0 "$out/perf" "$scenario"
  done
  "${CXX:-clang++}" "${common[@]}" -DPORT_RUNTIME_DIAGNOSTICS=0 -UPORT_LOGGING -DPORT_LOGGING=0 \
    "$root/tests/vita_perf_audit.cpp" -pthread -o "$out/quiet-perf"
  GXV_WORKLOAD_PROBE=0 "$out/quiet-perf" quiet
  exit
fi
[[ $part == all || $part == perf || $part == worker ]]
mounts=(--mount "type=bind,source=$root,target=/work,readonly"
        --mount "type=bind,source=$sdk,target=/sdk,readonly")
if test -n "${VITA_TURN_FIXTURE_DIR:-}"; then
  test -f "$VITA_TURN_FIXTURE_DIR/perf.cpp"
  test -f "$VITA_TURN_FIXTURE_DIR/os_thread.cpp"
  mounts+=(--mount "type=bind,source=$VITA_TURN_FIXTURE_DIR,target=/fixture,readonly")
fi
docker run --rm --pull=never --platform linux/arm64 --network none --cpus=2 \
  -e "TURN_TEST_PART=$part" \
  -e "TURN_TEST_SCENARIO=${VITA_TURN_SCENARIO:-}" \
  "${mounts[@]}" -w /work \
  codex-galaxy-native:20261007-arm64 bash -euo pipefail -c '
    export ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1
    common=(-std=c++20 -stdlib=libc++ -O1 -g -D__vita__ -DTARGET_PC=1 -DNDEBUG=1 -fms-extensions
      -ffunction-sections -fdata-sections -Wl,--gc-sections -Iplatform/include -Ivita/runtime
      -Idecomp/libs/RVL_SDK/include -idirafter /sdk/arm-vita-eabi/include
      -include platform/include/port/compat.h -Wno-register -Wno-unknown-pragmas
      -Wno-deprecated-volatile -Wno-deprecated-declarations
      -fsanitize=address,undefined -fno-sanitize-link-runtime -fno-omit-frame-pointer)
    if test -d /fixture; then
      common+=(-DVITA_PERF_SOURCE=\"/fixture/perf.cpp\" -DVITA_THREAD_SOURCE=\"/fixture/os_thread.cpp\")
    fi
    if test "$TURN_TEST_PART" != worker; then
      clang++ "${common[@]}" tests/vita_perf_audit.cpp -o /tmp/perf -l:libasan.so.8 -l:libubsan.so.1
      for scenario in turn-costs forced-costs fast-skip-costs double-done disabled-costs paced; do
        if test -z "$TURN_TEST_SCENARIO" || test "$TURN_TEST_SCENARIO" = "$scenario"; then
          timeout 10s /tmp/perf "$scenario"
        fi
      done
      clang++ "${common[@]}" -DPORT_RUNTIME_DIAGNOSTICS=0 -DPORT_LOGGING=0 \
        tests/vita_perf_audit.cpp -o /tmp/quiet-perf -l:libasan.so.8 -l:libubsan.so.1
      timeout 10s /tmp/quiet-perf quiet
    fi
    if test "$TURN_TEST_PART" != perf; then
      clang++ "${common[@]}" tests/vita_scheduling_audit.cpp -o /tmp/scheduler -l:libasan.so.8 -l:libubsan.so.1
      timeout 20s /tmp/scheduler --require-worker-log
      timeout 20s /tmp/scheduler --require-worker-log --fail-pin
      for diagnostic in 0 1; do
        for logging in 0 1; do
          if test "$diagnostic" = 1 && test "$logging" = 1; then continue; fi
          clang++ "${common[@]}" -DPORT_RUNTIME_DIAGNOSTICS=$diagnostic -DPORT_LOGGING=$logging \
            tests/vita_scheduling_audit.cpp -o /tmp/quiet-scheduler -l:libasan.so.8 -l:libubsan.so.1
          timeout 20s /tmp/quiet-scheduler --require-no-worker-log
        done
      done
    fi
  '
