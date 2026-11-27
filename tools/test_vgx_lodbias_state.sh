#!/usr/bin/env bash
# Native ASan/UBSan checks of LOD invalidation and real shader/uniform output; no GL or game assets.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
src="$root/platform/vgx/src"
"${CXX:-clang++}" -std=c++20 -O1 -g -Wall -Wextra -Werror -Wno-unused-function -Wno-unused-variable \
  -fsanitize=address,undefined -fno-omit-frame-pointer -DVGX_NO_NEON -DPORT_RUNTIME_DIAGNOSTICS=0 -DPORT_LOGGING=0 \
  -I"$src" -I"$root/platform/vgx/third_party" "$root/tests/vgx_lodbias_state.cpp" \
  "${VGX_STATE_SOURCE:-$src/vgx_state.cpp}" "$src/vgx_fesplit.cpp" "$src/vgx_dlcache.cpp" "$src/vgx_guestmem.cpp" \
  "$src/vgx_bpsync.cpp" "$src/vgx_shadergen.cpp" "$src/vgx_texcache.cpp" "$src/vgx_texdecode.cpp" "$src/vgx_backend_null.cpp" \
  -pthread -o "$out/vgx_lodbias_state"
export ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1
for enabled in 0 1; do
  GXV_FIX_LODBIAS="$enabled" GXV_RSCACHE=1 GXV_SPLIT=0 GXV_NO_SHADERLIST=1 "$out/vgx_lodbias_state" "$enabled" "${LOD_STATE_MODE:-check}"
  GXV_FIX_LODBIAS="$enabled" GXV_RSCACHE=1 GXV_SPLIT=0 GXV_NO_SHADERLIST=1 GXV_TEVWRAP=2 GXV_FIX_TEVIN=1 \
    GXV_FS_HALF=1 GXV_FOGVTX=2 GXV_NODISCARD=1 "$out/vgx_lodbias_state" "$enabled" "${LOD_STATE_MODE:-check}"
done
