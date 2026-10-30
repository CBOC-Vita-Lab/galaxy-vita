#!/usr/bin/env bash
# Host checks for the PGO build options (tools/pgo/README.md): build.py --pgo-profile, the VITA_PGO_PROFILE CMake
# option, and the armhf harness options (ARMHF_PGO_GEN / ARMHF_PGO_USE / ARMHF_DECOMP_BITCODE) at configure time.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
cd "$root"
python3 -m unittest tests.test_pgo_build -v
# armhf harness configure: default has no PGO flags; each option adds exactly its own.
configure() { cmake -S armhf -B "$tmp/$1" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$root/armhf/toolchain.cmake" -DCMAKE_BUILD_TYPE= "${@:2}" > "$tmp/$1.log" 2>&1 || { cat "$tmp/$1.log"; exit 1; }; }
count() { ninja -C "$tmp/$1" -t commands | grep -c -e "$2" || true; }
configure plain
[ "$(count plain 'fprofile\|flto')" = 0 ]
touch "$tmp/rt.a"; : > "$tmp/p.profdata"
configure gen -DARMHF_PGO_GEN=ON -DARMHF_PROFILE_RT="$tmp/rt.a"
[ "$(count gen 'fprofile-generate')" -gt 1800 ]
grep -q 'pgo_exit.cpp' "$tmp/gen/build.ninja"
grep -q -- '-u,__llvm_profile_runtime' "$tmp/gen/build.ninja"
configure use -DARMHF_PGO_USE="$tmp/p.profdata"
[ "$(count use 'fprofile-use=')" -gt 1800 ]
configure lto -DARMHF_DECOMP_BITCODE=ON
[ "$(count lto 'flto=thin')" -gt 1800 ]
grep -q '^build galaxy_lto.o' "$tmp/lto/build.ninja"
if cmake -S armhf -B "$tmp/bad" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$root/armhf/toolchain.cmake" -DARMHF_PGO_GEN=ON > "$tmp/bad.log" 2>&1; then
  echo "ARMHF_PGO_GEN without ARMHF_PROFILE_RT must fail"; exit 1
fi
echo "PGO build option checks passed"
