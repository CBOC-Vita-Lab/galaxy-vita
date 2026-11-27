#!/usr/bin/env bash
# c183fix F7: the hrw41 depth mark ends with its picture, with or without a display copy (tests/c183_depth_mark.cpp, the real front end
# with the null back end). REF_ROOT=<a tree without the fix>: the same test is built against that tree's front end and must FAIL.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
source tools/host_san.sh
cxx=${CXX:-c++}
san=($(host_san "$cxx" -fsanitize=address,undefined))
build() {   # <tree> <binary>
    local srcs=()
    for u in vgx_state vgx_draw vgx_fesplit vgx_dlcache vgx_guestmem vgx_bpsync vgx_shadergen vgx_texcache vgx_texcomp vgx_texdecode vgx_stats vgx_backend_null; do
        srcs+=("$1/platform/vgx/src/$u.cpp")
    done
    local link=(-Wl,--gc-sections)
    if [[ $(uname -s) == Darwin ]]; then link=(-Wl,-dead_strip); fi
    "$cxx" -std=c++20 -O1 -g -w -DVGX_NO_NEON -ffunction-sections -fdata-sections ${san[@]+"${san[@]}"} -I"$1/platform/include" -I"$1/platform/vgx/src" \
        -I"$1/platform/vgx/third_party" tests/c183_depth_mark.cpp "${srcs[@]}" "${link[@]}" -pthread -o "$2"
}
export ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1
build "$root" "$out/mark"
"$out/mark"
if [[ -n ${REF_ROOT:-} ]]; then
    build "$REF_ROOT" "$out/ref"
    if "$out/ref" > "$out/ref.txt"; then echo "FAIL: the test passes against $REF_ROOT too"; exit 1; fi
    grep FAIL "$out/ref.txt" || true
    echo "(negative control: $REF_ROOT fails, as it must)"
fi
