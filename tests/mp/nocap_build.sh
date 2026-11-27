#!/bin/bash
# MP3 nocap: the thread layout without the Vita's fourth core (no CapUnlocker) and with it (tests/mp/nocap_test.cpp), plus a static check
# that no multiplayer or net/ thread is made for, or moved to, the system core (mask 0x80000 / SCE_KERNEL_CPU_MASK_SYSTEM).
#   tests/mp/nocap_build.sh            build, run   (ASan + UBSan; -D__vita__ over tests/par_stubs: the workers' console branch)
set -eo pipefail
. "$(cd "$(dirname "$0")" && pwd)/tmproot.sh"
root=$(cd "$(dirname "$0")/../.." && pwd)
out=${MP_TEST_OUT:-$MP_TMP/mp_nocap_tests}
mkdir -p "$out"
if [ -z "$CXX" ]; then
  if echo 'int main(){}' | clang++ -fsanitize=address -x c++ - -o $MP_TMP/.mp_asan_probe 2>/dev/null; then CXX=clang++; else CXX=g++; fi
fi
nice -n 10 $CXX -std=c++20 -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined -Wall -Wextra -Wno-unused-parameter \
  -D__vita__ -I"$root/tests/par_stubs" -I"$root/platform/include" -o "$out/nocap_test" "$root/tests/mp/nocap_test.cpp" -lpthread
echo "built in $out"
[ "$1" = "build" ] && exit 0
"$out/nocap_test"
# static: every thread creation / affinity change in the session code and net/ names a user core (or none), never the system core
bad=$(grep -nE 'sceKernel(CreateThread|ChangeThreadCpuAffinityMask)' -r "$root/platform/src/mp" "$root/net/src" "$root/decomp/src/Game/MP" \
      | grep -E '0x80000\b|CPU_MASK_SYSTEM' || true)
if [ -n "$bad" ]; then echo "FAIL: a session / net thread on the system core:"; echo "$bad"; exit 1; fi
echo "static: no session / net thread asks for the system core: ok"
