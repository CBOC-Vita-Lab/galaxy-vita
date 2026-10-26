#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
cd "$root"
flags=(-std=c++17 -O1 -g -DVGX_NO_NEON -DPORT_LOGGING=0
       -fsanitize=address,undefined -fno-omit-frame-pointer -Iplatform/vgx/src -Iplatform/vgx/third_party)
if [[ ${1:-} == bench ]]; then
  flags=(-std=c++17 -O2 -DVGX_NO_NEON -DPORT_LOGGING=0
         -Iplatform/vgx/src -Iplatform/vgx/third_party)
fi
for diagnostic in ${POSITION_DEFAULT_DIAGNOSTICS:-0 1}; do
"${CXX:-clang++}" "${flags[@]}" -DPORT_RUNTIME_DIAGNOSTICS=$diagnostic tests/vgx_position_defaults.cpp \
  platform/vgx/src/vgx_draw.cpp platform/vgx/src/vgx_fesplit.cpp platform/vgx/src/vgx_dlcache.cpp \
  platform/vgx/src/vgx_guestmem.cpp platform/vgx/src/vgx_bpsync.cpp platform/vgx/src/vgx_shadergen.cpp \
  platform/vgx/src/vgx_texcache.cpp platform/vgx/src/vgx_texdecode.cpp platform/vgx/src/vgx_stats.cpp \
  platform/vgx/src/vgx_backend_null.cpp -o "$out/check"
export ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1
if [[ ${1:-} == bench ]]; then
  GXV_POSDEFAULT=1 "$out/check" bench
  exit
fi
for mode in ${POSITION_DEFAULT_MODES:-0 1 2 -1 3 invalid}; do
  printf 'diagnostics=%s position-mode=%s decode-mode=1\n' "$diagnostic" "$mode"
  GXV_POSDEFAULT=$mode "$out/check" "$mode"
done
printf 'diagnostics=%s position-mode=1 decode-mode=0\n' "$diagnostic"
GXV_POSDEFAULT=1 GXV_DECODE=0 "$out/check" 1
printf 'diagnostics=%s position-mode=2 decode-mode=2\n' "$diagnostic"
GXV_POSDEFAULT=2 GXV_DECODE=2 "$out/check" 2
done
