#!/usr/bin/env bash
# GXV_LODBIAS_SAMPLER (lodmask37): the pure decision logic (platform/vgx/src/vgx_lodsampler.h; tests/vgx_lodsampler_test.cpp), then the same switch through
# the real front end (tests/vgx_lodsampler_state.cpp: BP writes, state-list replay, key builder, draw records; no GL, no game data).
# Exit status 0 when every check passes. SAN=1 adds ASan + UBSan to the state test (slower; the toolchain needs its sanitizer runtime).   tools/test_vgx_lodsampler.sh [output-dir]
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
src="$root/platform/vgx/src"
"${CXX:-g++}" -std=gnu++20 -O1 -g -Wall -Wextra -fsanitize=undefined -I"$src" "$root/tests/vgx_lodsampler_test.cpp" -o "$out/vgx_lodsampler_test"
"$out/vgx_lodsampler_test"
"${CXX:-g++}" -std=c++20 -O1 -g -Wall -Wextra -Wno-unused-function -Wno-unused-variable -Wno-deprecated-enum-enum-conversion -Wno-sign-compare -Wno-unused-parameter \
  ${SAN:+-fsanitize=address,undefined -fno-omit-frame-pointer} -DVGX_NO_NEON -DPORT_RUNTIME_DIAGNOSTICS=0 -DPORT_LOGGING=0 \
  -I"$src" -I"$root/platform/include" -I"$root/platform/vgx/third_party" "$root/tests/vgx_lodsampler_state.cpp" \
  "$src/vgx_state.cpp" "$src/vgx_fesplit.cpp" "$src/vgx_dlcache.cpp" "$src/vgx_guestmem.cpp" \
  "$src/vgx_bpsync.cpp" "$src/vgx_shadergen.cpp" "$src/vgx_texcache.cpp" "$src/vgx_texdecode.cpp" "$src/vgx_texcomp.cpp" "$src/vgx_backend_null.cpp" \
  -pthread -o "$out/vgx_lodsampler_state"
export ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1
common="GXV_FIX_LODBIAS=1 GXV_RSCACHE=1 GXV_SPLIT=0 GXV_NO_SHADERLIST=1"
for mode in 0 1 2; do
  env $common GXV_LODBIAS_SAMPLER=$mode "$out/vgx_lodsampler_state" "$mode" plain
  env $common GXV_LODBIAS_SAMPLER=$mode GXV_RUNG_LODBIAS=100 "$out/vgx_lodsampler_state" "$mode" rung
  env $common GXV_LODBIAS_SAMPLER=$mode GXV_SPLIT=1 GXV_TEVWRAP=2 GXV_FIX_TEVIN=1 GXV_FS_HALF=1 GXV_FOGVTX=2 GXV_NODISCARD=1 "$out/vgx_lodsampler_state" "$mode" plain
done
# the switch needs GXV_FIX_LODBIAS: without it nothing moves and the front end behaves as before
env GXV_FIX_LODBIAS=0 GXV_RSCACHE=1 GXV_SPLIT=0 GXV_NO_SHADERLIST=1 GXV_LODBIAS_SAMPLER=1 "$out/vgx_lodsampler_state" off plain
