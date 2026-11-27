#!/usr/bin/env bash
# GXV_LA88_MIPS host proof (tests/vgx_la88_mips.cpp): the LA88 decode expanded = the RGBA8 decode for every I/IA format and size; the
# front end's choice per switch value and after the back end's "not exact". (The console half, the transfer unit's U8U8 downscale
# against U8U8U8U8, is checked on the console itself at the first such upload: vgx_host_galaxy_vita.cpp vgxvita_la88MipsExact.)
#   tools/test_vgx_la88_mips.sh       (BASE_ROOT=<full tree> when this tree is sparse)
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
link=(-Wl,--gc-sections)
if [[ $(uname -s) == Darwin ]]; then link=(-Wl,-dead_strip); fi
"${CXX:-c++}" -std=c++20 -O1 -g -DVGX_NO_NEON -ffunction-sections -fdata-sections -I"$root/platform/include" -I"$root/platform/vgx/src" -I"$base/platform/vgx/src" -I"$base/platform/vgx/third_party" \
    "$(pick tests/vgx_la88_mips.cpp)" "${srcs[@]}" "${link[@]}" -pthread -o "$out/vgx_la88_mips"
fail=0
for m in 0 1 2; do
    env GXV_LA88=1 GXV_MIPGEN=1 GXV_MIPCLAMP=1 GXV_LA88_MIPS=$m "$out/vgx_la88_mips" $m || fail=1
done
awk '/^extern "C" int vgxvita_la88MipsExact\(void\) \{/{copy=1;found++} copy {print} copy && /^}/ {copy=0} END{if(found!=1 || copy)exit 1}' \
    "$root/platform/vgx/vita/vgx_host_galaxy_vita.cpp" > "$out/probe.inc"
source "$root/tools/host_san.sh"
san=($(host_san "${CXX:-c++}" -fsanitize=address,undefined))
"${CXX:-c++}" -std=c++17 -O1 -g ${san[@]+"${san[@]}"} -I"$out" -I"$root/platform/vgx/vita" "$root/tests/c183_mip_probe.cpp" -o "$out/probe"
"$out/probe"
exit $fail
