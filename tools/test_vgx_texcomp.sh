#!/usr/bin/env bash
# GXV_TEX_COMPRESS host proof (tests/vgx_texcomp.cpp; texcomp23): the encoder, the front end with the switch off and on (null back end,
# the console's caps), and the card cache across two processes (the second reads every conversion back, byte for byte).
#   tools/test_vgx_texcomp.sh       (BASE_ROOT=<full tree> when this tree is sparse)
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
base=${BASE_ROOT:-$root}
pick() { if [ -f "$root/$1" ]; then echo "$root/$1"; else echo "$base/$1"; fi; }
out=${OUT:-$(mktemp -d)}
[ -z "${OUT:-}" ] && trap 'rm -rf "$out"' EXIT
srcs=()
for u in vgx_state vgx_draw vgx_fesplit vgx_dlcache vgx_guestmem vgx_bpsync vgx_shadergen vgx_texcache vgx_texcomp vgx_texdecode vgx_stats vgx_backend_null; do
    srcs+=("$(pick platform/vgx/src/$u.cpp)")
done
nice -n 10 "${CXX:-g++}" -std=c++17 -O1 -g -I"$root/platform/vgx/src" -I"$base/platform/vgx/src" -I"$base/platform/vgx/third_party" \
    "$(pick tests/vgx_texcomp.cpp)" "${srcs[@]}" -pthread -o "$out/vgx_texcomp"
common=(GXV_VITA=1 GXV_LA88=1 GXV_MIPGEN=1 GXV_MIPCLAMP=1 GXV_TEX_COMPRESS_CACHE="$out/card")
fail=0
env "${common[@]}" "$out/vgx_texcomp" || fail=1                                   # switch off (unset)
env "${common[@]}" GXV_TEX_COMPRESS=0 "$out/vgx_texcomp" || fail=1
env "${common[@]}" GXV_TEX_COMPRESS=perf GXV_AUTOQ_TARGET=30 "$out/vgx_texcomp" || fail=1   # perf outside Performance mode = off
env "${common[@]}" GXV_TEX_COMPRESS=1 "$out/vgx_texcomp" first "$out/digests" || fail=1
env "${common[@]}" GXV_TEX_COMPRESS=perf GXV_AUTOQ_TARGET=60 "$out/vgx_texcomp" second "$out/digests" || fail=1
env "${common[@]}" GXV_TEX_COMPRESS=1 GXV_TEX_COMPRESS_IA=1 "$out/vgx_texcomp" ia || fail=1         # I/IA too
env "${common[@]}" GXV_TEXCOMP_CENSUS=1 GXV_TEXCOMP_CENSUS_S=0 "$out/vgx_texcomp" census || fail=1 # the census lines
ls -l "$out"/card.* | awk '{print "card cache:", $5, $9}'
exit $fail
