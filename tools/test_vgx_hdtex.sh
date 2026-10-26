#!/usr/bin/env bash
# HD texture replacement through the real texture cache (native g++, no Docker): a synthetic
# one-texture pack written by tools/hdtex/make_test_pack.py (build_pack.py's key and writer),
# looked up by vgx_hdtex.cpp, for each mode of tests/vgx_hdtex.cpp.
#   tools/test_vgx_hdtex.sh [build dir, default /tmp/vgx-hdtex-test]
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-/tmp/vgx-hdtex-test}
mkdir -p "$out"
python3 -E -P "$root/tools/hdtex/make_test_pack.py" "$out/test.pack"
python3 -E -P "$root/tools/hdtex/make_test_pack.py" "$out/test-eye.pack" Test:SyntheticEye
${CXX:-g++} -std=c++20 -O1 -g -fsanitize=address,undefined -DVGX_NO_NEON -I"$root/platform/vgx/src" -I"$root/platform/vgx/third_party" \
  "$root/tests/vgx_hdtex.cpp" "$root/platform/vgx/src/vgx_hdtex.cpp" "$root/platform/vgx/src/vgx_state.cpp" "$root/platform/vgx/src/vgx_draw.cpp" "$root/platform/vgx/src/vgx_fesplit.cpp" \
  "$root/platform/vgx/src/vgx_dlcache.cpp" "$root/platform/vgx/src/vgx_guestmem.cpp" "$root/platform/vgx/src/vgx_bpsync.cpp" \
  "$root/platform/vgx/src/vgx_shadergen.cpp" "$root/platform/vgx/src/vgx_texcache.cpp" "$root/platform/vgx/src/vgx_texdecode.cpp" \
  "$root/platform/vgx/src/vgx_stats.cpp" "$root/platform/vgx/src/vgx_backend_null.cpp" -o "$out/vgx_hdtex" -lpthread
export ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1
for mode in replace clamp budget dryrun copy changed; do
  "$out/vgx_hdtex" "$out/test.pack" "$mode"
done
# eye textures (name contains "eye") keep the game's own image unless GXV_HDTEX_EYES=1
for mode in eyes eyeson; do
  "$out/vgx_hdtex" "$out/test-eye.pack" "$mode"
done
echo "PASS: 8 HD texture replacement scenarios"
