#!/usr/bin/env bash
# GXV_CAPREAD (water23, diagnostic): the screen captures' readers logged from the finished frame, through the production GX front end
# and GLES back end on Mesa (tests/vgx_capread_gl.cpp), at a render size that is not the EFB's. The pictures must be identical with the
# log on and off, and with GXV_EFBSWAP2 on and off; with the log on the readers before and after each capture are counted and the
# projective reader is named in the "capread:" lines.
#   tools/test_vgx_capread_gl.sh [output-dir]       (no game data, no Vita; Khronos headers: KHR_INCLUDE, default /work/khr/include)
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
src="$root/platform/vgx/src"
khr=${KHR_INCLUDE:-/work/khr/include}
sources=(vgx_state vgx_draw vgx_fesplit vgx_dlcache vgx_guestmem vgx_bpsync vgx_shadergen vgx_texcache vgx_texdecode vgx_stats vgx_backend_gl vgx_autoq)
args=()
for unit in "${sources[@]}"; do args+=("$src/$unit.cpp"); done
if [[ "${CAPREAD_SKIP_BUILD:-0}" != 1 ]]; then
  nice -n 10 "${CXX:-g++}" -std=c++20 -O1 -I"$src" -I"$root/platform/include" -I"$root/platform/vgx/third_party" -I"$khr" \
    "$root/tests/vgx_capread_gl.cpp" "${args[@]}" "$root/platform/vgx/host/vgx_host_egl.cpp" \
    -lEGL -lGLESv2 -pthread -o "$out/vgx_capread_gl"
fi
runs=("off0:GXV_CAPREAD=0 GXV_EFBSWAP2=0" "on0:GXV_CAPREAD=1 GXV_EFBSWAP2=0" "off1:GXV_CAPREAD=0 GXV_EFBSWAP2=1" "on1:GXV_CAPREAD=1 GXV_EFBSWAP2=1"
      "h1:GXV_CAPREAD=1 GXV_CAPREAD_HIDE=1" "h2:GXV_CAPREAD=1 GXV_CAPREAD_HIDE=2" "ab:GXV_CAPREAD=1 GXV_CAPREAD_INDFILT=1 GXV_CAPREAD_NOWARP=1")
for r in "${runs[@]}"; do
  v=${r%%:*}
  # shellcheck disable=SC2086
  env LIBGL_ALWAYS_SOFTWARE=1 LP_NUM_THREADS=1 GXV_LOG=0 GXV_SPLIT=0 GXV_AUTOQ=0 GXV_FXHALF=0 GXV_PASSHALF=0 GXV_CROWD=0 GXV_EFBSWAP=1 \
    GXV_CAPREAD_EVERY=1 ${r#*:} "$out/vgx_capread_gl" > "$out/capread-$v.log" 2>&1 || true
  grep -v "^libunwind" "$out/capread-$v.log" | grep -v "^\[vgx\] capread:" | sed "s/^/[$v] /"
done
grep -q FAIL "$out"/capread-*.log && { echo "FAILED: a check"; exit 1; }
pics() { grep -E '^frame[0-9]+ ' "$out/capread-$1.log"; }
for v in on0 off1 on1 ab; do [[ "$(pics off0)" == "$(pics $v)" ]] || { echo "FAILED: pictures differ (off0 vs $v)"; diff <(pics off0) <(pics $v) || true; exit 1; }; done
[[ "$(pics h1)" != "$(pics off0)" && "$(pics h2)" != "$(pics off0)" ]] || { echo "FAILED: GXV_CAPREAD_HIDE changed no picture"; exit 1; }
grep -q "left out: GXV_CAPREAD_HIDE" "$out/capread-h1.log" || { echo "FAILED: the left-out readers are not marked in the log"; exit 1; }
grep -q "capread: .*0:t0 s0 proj" "$out/capread-on0.log" || { echo "FAILED: the projective reader before the capture is not named"; exit 1; }
grep -q "BEFORE draw" "$out/capread-on1.log" || { echo "FAILED: no reader before the capture logged"; exit 1; }
! grep -q "capread:" "$out/capread-off0.log" || { echo "FAILED: log lines with GXV_CAPREAD=0"; exit 1; }
echo "sample log lines:"; grep -m 3 "capread:" "$out/capread-on1.log" | cut -c1-400
echo "ok: GXV_CAPREAD reads only (pictures identical with it on/off, GXV_EFBSWAP2 on/off, 96x72 for 64x64); readers before the capture see exactly the previous picture's capture (GX); GXV_CAPREAD_HIDE=1/2 leave out the readers before / after"
