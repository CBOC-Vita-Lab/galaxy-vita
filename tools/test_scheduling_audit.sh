#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
sdk=${VITASDK:-/opt/vitasdk}
expected_pin=${AUDIT_EXPECT_GAME_PIN:-0}
[[ $expected_pin == 0 || $expected_pin == 1 ]]
mkdir -p "$root/local/scheduling-audit"
docker run --rm --pull=never --platform linux/arm64 --network none --cpus=2 \
  -e "AUDIT_EXPECT_GAME_PIN=$expected_pin" \
  --mount "type=bind,source=$root,target=/work,readonly" \
  --mount "type=bind,source=$root/local/scheduling-audit,target=/results" \
  --mount "type=bind,source=$sdk,target=/sdk,readonly" -w /work \
  codex-galaxy-native:20261007-arm64 bash -euo pipefail -c '
    export ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1
    common=(-std=c++20 -stdlib=libc++ -O1 -g -D__vita__ -DTARGET_PC=1 -DNDEBUG=1 -fms-extensions
      -ffunction-sections -fdata-sections -Wl,--gc-sections -Iplatform/include -Ivita/runtime
      -Idecomp/libs/RVL_SDK/include -idirafter /sdk/arm-vita-eabi/include
      -include platform/include/port/compat.h -Wno-register -Wno-unknown-pragmas
      -Wno-deprecated-volatile -Wno-deprecated-declarations
      -fsanitize=address,undefined -fno-sanitize-link-runtime -fno-omit-frame-pointer)
    clang++ "${common[@]}" tests/vita_scheduling_audit.cpp -o /results/scheduler -l:libasan.so.8 -l:libubsan.so.1
    clang++ "${common[@]}" tests/vita_perf_audit.cpp -o /results/perf -l:libasan.so.8 -l:libubsan.so.1
    timeout 20s /results/scheduler --handoffs
    for scenario in cpu-scope cpu-failure stale-player forced-count 16000000 16600000 16683350 16700000 17000000 18100000 20000000 cheap-skips backlog; do
      timeout 10s /results/perf "$scenario"
    done
    for limit in 0 1 3 8; do
      PETARI_FRAMESKIP=$limit timeout 10s /results/perf cost-model 50000000 12000000
    done
    for drawn in 20000000 34000000 50000000; do
      for skipped in 8000000 12000000 18000000; do
        PETARI_FRAMESKIP=3 timeout 10s /results/perf cost-model "$drawn" "$skipped"
      done
    done
    for scenario in cpu-scope stale-player forced-count; do
      timeout 10s /results/perf "$scenario" --strict
    done
    set +e
    timeout 10s /results/scheduler --require-pin
    result=$?
    set -e
    expected_exit=$((1 - AUDIT_EXPECT_GAME_PIN))
    printf "Core-0 startup contract exit: %s (expected %s)\n" "$result" "$expected_exit"
    test "$result" = "$expected_exit"
    if test "$AUDIT_EXPECT_GAME_PIN" = 1; then
      timeout 20s /results/scheduler --require-pin --require-log
      timeout 20s /results/scheduler --require-log --fail-pin
      timeout 20s /results/scheduler --require-pin --require-log --fail-info
      for trial in 1 2 3 4 5; do
        printf "Pinned handoff trial %s\n" "$trial"
        timeout 20s /results/scheduler --require-pin --handoffs
      done
    fi
  '
