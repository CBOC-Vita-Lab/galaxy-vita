#!/usr/bin/env bash
# texwarm30: the real texture cache on the null back end, two visits of a stage at different addresses: identity key vs the old address key.  bash tools/test_texwarm_id_replay.sh [out-dir]
# (compiles 12 renderer files: slow on a small machine; run it alone, nice)
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d "${TMPDIR:-/tmp}/texwarm_replay.XXXXXX")}
mkdir -p "$out"
S="$root/platform/vgx/src"
"${CXX:-g++}" -std=gnu++17 -O0 -w -DVGX_NO_NEON -I"$root/platform/include" -I"$S" -I"$root/platform/vgx/third_party" \
  "$root/tests/texwarm_id_replay.cpp" "$S/vgx_state.cpp" "$S/vgx_draw.cpp" "$S/vgx_fesplit.cpp" "$S/vgx_dlcache.cpp" "$S/vgx_guestmem.cpp" "$S/vgx_bpsync.cpp" "$S/vgx_shadergen.cpp" \
  "$S/vgx_texcache.cpp" "$S/vgx_texdecode.cpp" "$S/vgx_stats.cpp" "$S/vgx_backend_null.cpp" -o "$out/texwarm_id_replay" -lpthread
"$out/texwarm_id_replay" key
"$out/texwarm_id_replay" addr
