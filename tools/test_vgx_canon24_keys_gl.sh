#!/usr/bin/env bash
# canon24b: GXV_TEVIN_CANON=2 against =1 on the real fragment keys of the game's shader lists (tests/vgx_canon24_keys_gl.cpp): texts,
# pictures on Mesa and the witness search, under the console's shader switches (vita/vgx-game/entry.cpp) and three other sets.
# (Where the GLES / EGL headers are not installed: CPLUS_INCLUDE_PATH=<khronos include dir>.)
#   tools/test_vgx_canon24_keys_gl.sh [--asan] <output-dir> <vgx_shaderlist.bin | dir of raw .key files>... [-- --pack <stages.pack> ...]
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
san=()
if [ "${1:-}" = "--asan" ]; then san=(-fsanitize=address,undefined -fno-omit-frame-pointer); shift; fi
out=$1; shift
mkdir -p "$out"
lists=(); extra=()
while [ $# -gt 0 ]; do
  if [ "$1" = "--" ]; then shift; extra=("$@"); break; fi
  lists+=("$1"); shift
done
"${CXX:-g++}" -std=c++20 -O1 -g -Wall -Wextra "${san[@]}" -I"$root/platform/vgx/src" -I"$root/platform/vgx/third_party" -I"$root/tests" \
  "$root/tests/vgx_canon24_keys_gl.cpp" "$root/platform/vgx/src/vgx_shadergen.cpp" "$root/platform/vgx/host/vgx_host_egl.cpp" -lEGL -lGLESv2 \
  -o "$out/vgx_canon24_keys_gl"
console="GXV_FS_HALF=1 GXV_FS_TRIM=1 GXV_FS_TRIM_PARTS=47 GXV_TEVWRAP=2 GXV_NODISCARD=1 GXV_FIX_TEVIN=1 GXV_FIX_INDPREV=1 GXV_FIX_TEXOFF=1
 GXV_FIX_LODBIAS=1 GXV_FIX_DSTA=1 GXV_FIX_LOGICOP=1 GXV_FOGVTX=2 GXV_FOG_BIND=2 GXV_FS_FINCLAMP=1 GXV_DISCARD_BLEND=1 GXV_IND_VS=1 GXV_TEXVEC2=1
 GXV_VSOUT=1 GXV_IND_PRECOMP=0"
fail=0
i=0
for env in "$console" "$console GXV_FS_TEXREUSE=1" "$console GXV_FS_TRIM=0" "GXV_FIX_TEVIN=1"; do
  i=$((i + 1))
  echo "== set $i: $(echo $env | tr -s ' \n' ' ')"
  # shellcheck disable=SC2086
  if ! env $env LIBGL_ALWAYS_SOFTWARE=1 ASAN_OPTIONS=detect_leaks=0 "$out/vgx_canon24_keys_gl" "${extra[@]}" --names "$out/names-$i.tsv" "${lists[@]}" \
        > "$out/set-$i.txt" 2> "$out/set-$i.err"; then fail=1; fi
  grep -v '^source\|^  key' "$out/set-$i.txt" || true
  echo "keys listed with a difference or a witness: $(grep -c '^  key' "$out/set-$i.txt" || true)"
  grep '^FAIL' "$out/set-$i.err" | head -5 || true
done
[ "$fail" = 0 ] && echo "ok: GXV_TEVIN_CANON=2 on the real keys" || { echo "FAILED"; exit 1; }
