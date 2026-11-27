#!/usr/bin/env bash
# GXV_TEX_COMPRESS=0 is the old front end, byte for byte (tests/vgx_texcomp_identity.cpp; texcomp23): the same driver built against a tree
# WITHOUT the switch (argument: its root, e.g. a checkout of the base commit) and against this tree, run with the switch unset / 0 / the
# census on; every digest must be the same.
#   tools/test_vgx_texcomp_identity.sh <base tree root>
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
old=$(cd "$1" && pwd)
out=${OUT:-$(mktemp -d)}
[ -z "${OUT:-}" ] && trap 'rm -rf "$out"' EXIT
build() { # <tree> <binary> <extra units...>
    local tree=$1 bin=$2; shift 2
    local srcs=()
    for u in vgx_state vgx_draw vgx_fesplit vgx_dlcache vgx_guestmem vgx_bpsync vgx_shadergen vgx_texcache vgx_texdecode vgx_stats vgx_backend_null "$@"; do
        srcs+=("$tree/platform/vgx/src/$u.cpp")
    done
    nice -n 10 "${CXX:-g++}" -std=c++17 -O1 -I"$tree/platform/vgx/src" -I"$tree/platform/vgx/third_party" "$root/tests/vgx_texcomp_identity.cpp" \
        "${srcs[@]}" -pthread -o "$out/$bin"
}
build "$old" old
build "$root" new vgx_texcomp
common=(GXV_VITA=1 GXV_LA88=1 GXV_MIPGEN=1 GXV_MIPCLAMP=1 GXV_TEX_COMPRESS_CACHE=0)
ref=$(env "${common[@]}" "$out/old")
echo "base tree:              $ref"
fail=0
for v in "" "GXV_TEX_COMPRESS=0" "GXV_TEXCOMP_CENSUS=1" "GXV_TEX_COMPRESS=perf GXV_AUTOQ_TARGET=30"; do
    got=$(env "${common[@]}" $v "$out/new")
    echo "this tree ${v:-(unset)}: $got"
    [ "$got" == "$ref" ] || { echo "FAIL: differs"; fail=1; }
done
# (and the switch on must differ only by its later replacements: the first uploads are the same; tests/vgx_texcomp.cpp checks that)
[ $fail == 0 ] && echo "vgx_texcomp_identity: PASS" || echo "vgx_texcomp_identity: FAIL"
exit $fail
