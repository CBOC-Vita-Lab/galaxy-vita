#!/usr/bin/env bash
# pix23: GXV_PIX_ZERO / GXV_PIX_ATPASS on Mesa (no game data, no Vita).
#   1. tests/vgx_pixval_gl.cpp: FsValueEval's claims (colour / alpha exactly 0, the alpha test passing / failing for every fragment) against
#      what random generated shaders draw with random inputs, under the console's shader switches and the plain generator.
#   2. tests/vgx_pixfe_gl.cpp: the production front end and GLES back end; pictures identical (all channels) with the switches off, on and
#      counting only, under the plain generator and the console's shader switches; the counts must be the frames' own.
#   tools/test_vgx_pixval_gl.sh [output-dir] [keys]      (Khronos headers: KHR_INCLUDE, default /work/khr/include)
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
keys=${2:-3000}
mkdir -p "$out"
src="$root/platform/vgx/src"
khr=${KHR_INCLUDE:-/work/khr/include}
console="GXV_FS_HALF=1 GXV_FS_TRIM=1 GXV_FS_TRIM_PARTS=47 GXV_TEVWRAP=2 GXV_NODISCARD=1 GXV_DISCARD_BLEND=1 GXV_FIX_INDPREV=1 GXV_FOGVTX=2 GXV_FIX_DSTA=1 GXV_FIX_TEXOFF=1"
fail=0
if [[ "${PIXVAL_SKIP_BUILD:-0}" != 1 ]]; then
  nice -n 10 "${CXX:-g++}" -std=c++20 -O1 -g -I"$src" -I"$root/platform/vgx/third_party" -I"$root/tests" -I"$khr" \
    "$root/tests/vgx_pixval_gl.cpp" "$src/vgx_shadergen.cpp" "$root/platform/vgx/host/vgx_host_egl.cpp" -lEGL -lGLESv2 -o "$out/vgx_pixval_gl"
  sources=(vgx_state vgx_draw vgx_fesplit vgx_dlcache vgx_guestmem vgx_bpsync vgx_shadergen vgx_texcache vgx_texdecode vgx_texcomp vgx_stats vgx_backend_gl vgx_autoq)
  args=()
  for unit in "${sources[@]}"; do args+=("$src/$unit.cpp"); done
  nice -n 10 "${CXX:-g++}" -std=c++20 -O1 -g -I"$src" -I"$root/platform/include" -I"$root/platform/vgx/third_party" -I"$khr" \
    "$root/tests/vgx_pixfe_gl.cpp" "${args[@]}" "$root/platform/vgx/host/vgx_host_egl.cpp" -lEGL -lGLESv2 -pthread -o "$out/vgx_pixfe_gl"
fi
# 1
for envset in "$console" "GXV_FS_FINCLAMP=1 GXV_FS_PROJ=1 $console" "GXV_NODISCARD=0"; do
  env $envset LIBGL_ALWAYS_SOFTWARE=1 "$out/vgx_pixval_gl" "$keys" > "$out/pixval.log" 2>&1 || true
  echo "pixval [$envset]: $(grep -E '^pixval|^FAIL' "$out/pixval.log" | head -2 | tr '\n' ' ')"
  grep -q '^PASS' "$out/pixval.log" || { fail=1; grep -v '^libunwind' "$out/pixval.log" | tail -60; }
done
# 2
for envset in "" "$console"; do
  ref=""
  for sw in "0 0" "1 0" "2 0" "3 0" "0 1" "0 2" "1 1" "2 2"; do  # (defaults48: 3 = the lean way, alone)
    read -r z a <<< "$sw"
    tag="z$z-a$a"
    env $envset LIBGL_ALWAYS_SOFTWARE=1 LP_NUM_THREADS=1 GXV_LOG=0 GXV_SPLIT=0 GXV_AUTOQ=0 GXV_FXHALF=0 GXV_PASSHALF=0 GXV_CROWD=0 \
      GXV_PIX_ZERO=$z GXV_PIX_ATPASS=$a "$out/vgx_pixfe_gl" > "$out/pixfe-$tag.log" 2>&1 || fail=1
    grep -v "^libunwind" "$out/pixfe-$tag.log" | grep -E "FAIL|^counts" | sort -u | sed "s/^/${envset:+console }$tag: /"
    h=$(grep -E '^frame[0-9] hash' "$out/pixfe-$tag.log" | tr '\n' ' ')
    if [[ -z "$ref" ]]; then ref="$h"; echo "${envset:+console }$tag: $h"; elif [[ "$h" != "$ref" ]]; then echo "FAILED: $tag pictures differ: $h vs $ref"; fail=1; fi
    grep -q FAIL "$out/pixfe-$tag.log" && fail=1
  done
  # (heavyopt25: GXV_PIX_CENSUS=2 / GXV_PIX_BLENDOFF=1 alone switch the texture channel ranges on: the proofs may now use them; pictures must not change)
  for extra in "GXV_PIX_CENSUS=2" "GXV_PIX_BLENDOFF=1" "GXV_PIX_CENSUS=2 GXV_PIX_BLENDOFF=1 GXV_PIX_ATPASS=1 GXV_PIX_ZERO=1"; do
    tag="x-${extra// /_}"
    env $envset LIBGL_ALWAYS_SOFTWARE=1 LP_NUM_THREADS=1 GXV_LOG=0 GXV_SPLIT=0 GXV_AUTOQ=0 GXV_FXHALF=0 GXV_PASSHALF=0 GXV_CROWD=0 $extra "$out/vgx_pixfe_gl" > "$out/pixfe-$tag.log" 2>&1 || fail=1
    h=$(grep -E '^frame[0-9] hash' "$out/pixfe-$tag.log" | tr '\n' ' ')
    if [[ "$h" != "$ref" ]]; then echo "FAILED: $tag pictures differ: $h vs $ref"; fail=1; else echo "${envset:+console }$tag: identical"; fi
    grep -q FAIL "$out/pixfe-$tag.log" && fail=1
  done
done
[[ $fail -eq 0 ]] || { echo "FAILED"; exit 1; }
echo "PASS: FsValueEval's claims hold on Mesa; identical pictures with GXV_PIX_ZERO / GXV_PIX_ATPASS off, on and counting, cuts made"
