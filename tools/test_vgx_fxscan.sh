#!/usr/bin/env bash
# Linux host dispatch checks use the real backend, but never create a GL context.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
src="$root/platform/vgx/src"
for diagnostics in 0 1; do
  "${CXX:-clang++}" -std=c++20 -O1 -g -DPORT_RUNTIME_DIAGNOSTICS=$diagnostics \
    -I"$src" -I"$root/platform/vgx/third_party" \
    "$root/tests/vgx_fxscan_test.cpp" "$src/vgx_autoq.cpp" "$src/vgx_shadergen.cpp" \
    "$root/platform/vgx/host/vgx_host_egl.cpp" -lEGL -lGLESv2 -lpthread -o "$out/fxscan-$diagnostics"
done
run() {
  local setting=$1 want=$2 workload=${3-0} observed=${4-0} fx=${5-0} diagnostics=${6-0} kind=${7-ordinary}
  if test "$setting" = unset; then
    env -u GXV_FXSCANFAST GXV_WORKLOAD_PROBE="$workload" GXV_FXHALF="$fx" "$out/fxscan-$diagnostics" "$want" "$observed" "$kind"
  else
    GXV_FXSCANFAST="$setting" GXV_WORKLOAD_PROBE="$workload" GXV_FXHALF="$fx" "$out/fxscan-$diagnostics" "$want" "$observed" "$kind"
  fi
}
run 1 1
for setting in unset 0 2 invalid -1 3 99999999999999999999999999999999999; do run "$setting" 0; done
run 1suffix 1 # EnvInt's established strtol-prefix semantics.
run 1 0 1 1
for workload in '' 00 invalid 99999999999999999999999999999999999; do run 1 0 "$workload"; done
run 1 0 0 0 2
run 1 0 0 0 4
run 1 0 0 0 0 1
run 2 0 1 1
run 1 0 1 1 0 0 initialized
run 1 0 99999999999999999999999999999999999 0 0 0 empty
run 1 1 0 0 0 0 empty
env -u GXV_WORKLOAD_PROBE GXV_FXSCANFAST=1 GXV_FXHALF=0 "$out/fxscan-0" 1 0
run 2 0 0 0 0 0 oracle > "$out/oracle.log" 2>&1
test "$(grep -c 'empty-mask mismatch' "$out/oracle.log")" = 3
grep '^PASS:' "$out/oracle.log"
printf '%s\n' 'PASS: oracle mismatch logging is bounded to three lines'
