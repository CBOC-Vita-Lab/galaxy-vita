#!/usr/bin/env bash
# canon24b: GXV_TEVIN_CANON=2's front-end half through the production units and the GLES back end on Mesa (tests/vgx_canon24_fe_gl.cpp): the
# register mask, BuildTevC's pre-wrapped upload, the wrap's float arithmetic, and pictures of every markable stage configuration of the
# key lists, =1 against =2 (and GXV_SPLIT 0 / 1 / 2, GXV_RSCACHE=0), compared byte for byte.
# (Where the GLES / EGL headers are not installed: CPLUS_INCLUDE_PATH=<khronos include dir>.)
#   tools/test_vgx_canon24_fe_gl.sh [--asan] <output-dir> <vgx_shaderlist.bin | dir of raw .key files>...
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
san=()
if [ "${1:-}" = "--asan" ]; then san=(-fsanitize=address,undefined -fno-omit-frame-pointer); shift; fi
out=$1; shift
mkdir -p "$out"
src="$root/platform/vgx/src"
units=(vgx_state vgx_draw vgx_fesplit vgx_dlcache vgx_guestmem vgx_bpsync vgx_shadergen vgx_texcache vgx_texdecode vgx_stats vgx_backend_gl vgx_autoq
       vgx_texcomp)
args=()
for unit in "${units[@]}"; do args+=("$src/$unit.cpp"); done
"${CXX:-g++}" -std=c++20 -O1 -g "${san[@]}" -I"$src" -I"$root/platform/vgx/third_party" -I"$root/platform/include" \
  "$root/tests/vgx_canon24_fe_gl.cpp" "${args[@]}" "$root/platform/vgx/host/vgx_host_egl.cpp" -lEGL -lGLESv2 -pthread -o "$out/vgx_canon24_fe_gl"
console="GXV_FS_HALF=1 GXV_FS_TRIM=1 GXV_FS_TRIM_PARTS=47 GXV_TEVWRAP=2 GXV_NODISCARD=1 GXV_FIX_TEVIN=1 GXV_FIX_INDPREV=1 GXV_FIX_TEXOFF=1
 GXV_FIX_LODBIAS=1 GXV_FIX_DSTA=1 GXV_FIX_LOGICOP=1 GXV_FOGVTX=2 GXV_FOG_BIND=2 GXV_FS_FINCLAMP=1 GXV_DISCARD_BLEND=1 GXV_IND_VS=1 GXV_TEXVEC2=1
 GXV_VSOUT=1"
fail=0
run() { # <name> <env...>
  local name=$1; shift
  # shellcheck disable=SC2086
  if ! env $console "$@" LIBGL_ALWAYS_SOFTWARE=1 LP_NUM_THREADS=1 GXV_LOG=0 GXV_AUTOQ=0 GXV_FXHALF=0 GXV_PASSHALF=0 GXV_CROWD=0 GXV_EFBSWAP=0 \
        ASAN_OPTIONS=detect_leaks=0 "$out/vgx_canon24_fe_gl" "$out/pic-$name.bin" "${lists[@]}" > "$out/run-$name.txt" 2>&1; then
    echo "FAIL run $name"; fail=1
  fi
  grep -E '^(PASS|FAIL|mask|upload|arithmetic|pictures|GXV_TEVIN)' "$out/run-$name.txt" | sed "s/^/[$name] /"
}
lists=("$@")
for sp in 0 1 2; do
  run "c1-s$sp" GXV_TEVIN_CANON=1 GXV_SPLIT=$sp
  run "c2-s$sp" GXV_TEVIN_CANON=2 GXV_SPLIT=$sp
done
run "c2-rs0" GXV_TEVIN_CANON=2 GXV_RSCACHE=0
run "c0" GXV_TEVIN_CANON=0
cmpic() { # <a> <b>: bytes and pixels that differ, the largest difference
  python3 -I - "$out/pic-$1.bin" "$out/pic-$2.bin" <<'EOF'
import sys
a = open(sys.argv[1], 'rb').read(); b = open(sys.argv[2], 'rb').read()
if len(a) != len(b) or not a: print("sizes differ: %d %d" % (len(a), len(b))); sys.exit(1)
d = [abs(x - y) for x, y in zip(a, b)]
px = sum(1 for i in range(0, len(d), 4) if max(d[i:i + 4]))
print("%d pictures, %d bytes, %d differ, %d pixels, max %d levels" % (len(a) // (64 * 64 * 4), len(a), sum(1 for x in d if x), px, max(d)))
sys.exit(0 if max(d) <= 2 else 1)
EOF
}
for pair in "c1-s0 c2-s0" "c1-s1 c2-s1" "c1-s2 c2-s2" "c2-s0 c2-s1" "c2-s0 c2-s2" "c2-s0 c2-rs0" "c0 c1-s0"; do
  set -- $pair
  printf '%s vs %s: ' "$1" "$2"
  cmpic "$1" "$2" || fail=1
done
[ "$fail" = 0 ] && echo "ok: GXV_TEVIN_CANON=2 front end" || { echo "FAILED"; exit 1; }
