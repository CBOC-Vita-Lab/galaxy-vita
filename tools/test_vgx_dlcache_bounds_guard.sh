#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
cd "$root"
export ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1
for diagnostics in ${GXV_BOUNDS_DIAGNOSTICS:-0 1}; do
  "${CXX:-clang++}" -std=c++17 -O1 -g -DVGX_NO_NEON -DPORT_LOGGING=0 \
    -DPORT_RUNTIME_DIAGNOSTICS="$diagnostics" -fsanitize=address,undefined -fno-omit-frame-pointer \
    -Iplatform/vgx/src -Iplatform/vgx/third_party tests/vgx_dlcache_bounds_guard.cpp \
    platform/vgx/src/vgx_state.cpp platform/vgx/src/vgx_draw.cpp platform/vgx/src/vgx_fesplit.cpp \
    platform/vgx/src/vgx_dlcache.cpp platform/vgx/src/vgx_guestmem.cpp \
    platform/vgx/src/vgx_bpsync.cpp platform/vgx/src/vgx_shadergen.cpp \
    platform/vgx/src/vgx_texcache.cpp platform/vgx/src/vgx_texdecode.cpp \
    platform/vgx/src/vgx_stats.cpp platform/vgx/src/vgx_backend_null.cpp -o "$out/check"
  if [[ ${1:-} == red ]]; then
    GXV_DLCACHE_BOUNDS_GUARD=1 GXV_DLCACHE=1 GXV_STATICBUF=1 "$out/check" on unmapped
    exit
  fi
  cases=0
  for cache in 0 1 verify; do
    for static in 0 1; do
      for scenario in unmapped partial shrink valid-rebind valid direct; do
        for mode in unset 0 1; do
          expected=off
          [[ "$mode" != 1 ]] || expected=on
          log="$out/$diagnostics-$cache-$static-$scenario-$mode.log"
          if [[ "$mode" == unset ]]; then
            env -u GXV_DLCACHE_BOUNDS_GUARD GXV_DLCACHE="$cache" GXV_STATICBUF="$static" \
              "$out/check" "$expected" "$scenario" > "$log" 2>&1
          else
            GXV_DLCACHE_BOUNDS_GUARD="$mode" GXV_DLCACHE="$cache" GXV_STATICBUF="$static" \
              "$out/check" "$expected" "$scenario" > "$log" 2>&1
          fi
          cat "$log"
          cases=$((cases + 1))
        done
        cmp "$out/$diagnostics-$cache-$static-$scenario-unset.log" "$out/$diagnostics-$cache-$static-$scenario-0.log"
      done
      for mode in 0 1; do
        expected=off
        [[ "$mode" != 1 ]] || expected=on
        for scenario in unmapped partial shrink; do
          GXV_DLCACHE_BOUNDS_GUARD="$mode" GXV_DLCACHE="$cache" GXV_STATICBUF="$static" \
            "$out/check" "$expected" "$scenario" flip
          cases=$((cases + 1))
        done
      done
    done
  done
  printf 'PASS: diagnostics=%s, %s bounds-guard cases; default/OFF exact parity and read-once behavior (host correctness only)\n' "$diagnostics" "$cases"
done
