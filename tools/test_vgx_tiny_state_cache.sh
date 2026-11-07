#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
src="$root/platform/vgx/src"
sources=("$root/tests/vgx_tiny_state_cache.cpp")
for unit in vgx_state vgx_draw vgx_fesplit vgx_guestmem vgx_bpsync vgx_shadergen vgx_texcache vgx_texdecode vgx_stats vgx_backend_null; do
  sources+=("$src/$unit.cpp")
done
export ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1
for diagnostics in 0 1; do
  "${CXX:-clang++}" -std=c++20 -O1 -g -DVGX_NO_NEON -DPORT_LOGGING=1 -DPORT_RUNTIME_DIAGNOSTICS=$diagnostics \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I"$src" -I"$root/platform/vgx/third_party" "${sources[@]}" -pthread -o "$out/tiny-state-$diagnostics"
  run() {
    env GXV_NO_SHADERLIST=1 GXV_ATLAS=0 GXV_SPLIT=0 GXV_RSCACHE=1 GXV_FIX_LODBIAS=1 GXV_FIX_TEVIN=1 \
      GXV_DLCACHE=1 GXV_STATELIST=1 GXV_DL_TINY=64 GXV_BPFAST=1 GXV_REGSHADOW=1 GXV_TRACE=-1 GXV_TRACE_DRAWS=0 \
      GXV_WORKLOAD_PROBE=0 GXV_ANIM_CATEGORIES=0 GXV_ANIM_FAMILIES=0 "$@"
  }
  run "$out/tiny-state-$diagnostics" all cached
  run GXV_BPFAST=0 GXV_REGSHADOW=0 "$out/tiny-state-$diagnostics" all cached
  run GXV_STATELIST=2 "$out/tiny-state-$diagnostics" all cached
  run GXV_SLFAST=0 "$out/tiny-state-$diagnostics" all cached
  run GXV_WORKLOAD_PROBE=1 GXV_ANIM_CATEGORIES=1 GXV_ANIM_FAMILIES=1 "$out/tiny-state-$diagnostics" all cached
  run GXV_DLCACHE=0 "$out/tiny-state-$diagnostics" population raw
  run GXV_STATELIST=0 "$out/tiny-state-$diagnostics" population raw
  run GXV_DLCACHE=verify "$out/tiny-state-$diagnostics" population cached
  expected=raw
  if [[ $diagnostics == 0 ]]; then expected=cached; fi
  run GXV_TRACE=1 "$out/tiny-state-$diagnostics" population "$expected"
done
echo 'PASS: tiny-state actual-source matrix diagnostics0/1; no game assets or Vita timing'
