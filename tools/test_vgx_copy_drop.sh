#!/usr/bin/env bash
# polish23 (8 Nov): GXV_COPY_DROP's texture-cache part (tests/vgx_copy_drop.cpp), real front end, null back end, ASan + UBSan.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
cd "$root"
export ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1
"${CXX:-clang++}" -std=c++17 -O1 -g -DVGX_NO_NEON -DPORT_LOGGING=0 -DPORT_RUNTIME_DIAGNOSTICS=0 -fsanitize=address,undefined -fno-omit-frame-pointer \
  -Iplatform/vgx/src -Iplatform/vgx/third_party tests/vgx_copy_drop.cpp \
  platform/vgx/src/vgx_state.cpp platform/vgx/src/vgx_draw.cpp platform/vgx/src/vgx_fesplit.cpp \
  platform/vgx/src/vgx_dlcache.cpp platform/vgx/src/vgx_guestmem.cpp \
  platform/vgx/src/vgx_bpsync.cpp platform/vgx/src/vgx_shadergen.cpp \
  platform/vgx/src/vgx_texcache.cpp platform/vgx/src/vgx_texdecode.cpp \
  platform/vgx/src/vgx_stats.cpp platform/vgx/src/vgx_backend_null.cpp platform/vgx/src/vgx_texcomp.cpp -o "$out/check"
"$out/check"
