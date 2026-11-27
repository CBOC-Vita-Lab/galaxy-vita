#!/bin/bash
# Host test of PETARI_VITA_LIMITS (platform/src/os/os_mem.cpp): the desktop memory banks are the console's 24 + 64 MiB when it is set and the
# desktop's 128 + 128 MiB when it is not. Compiles os_mem.cpp with a probe (no game, ~20 s), runs it in both address modes.
#   bash tests/mp/vita_limits_test.sh
set -euo pipefail
. "$(cd "$(dirname "$0")" && pwd)/tmproot.sh"
root=$(cd "$(dirname "$0")/../.." && pwd)
work=$(mktemp -d $MP_TMP/vita_limits.XXXX)
trap 'rm -rf "$work"' EXIT
FL=(-DNDEBUG=1 -DPORT_PLATFORM_CODE=1 -DTARGET_PC=1 -I$root/platform/include -I$root/decomp/include -I$root/decomp/libs/RVL_SDK/include
    -I$root/decomp/libs/JSystem/include -O1 -std=gnu++20 -mcmodel=large -fno-pie -stdlib=libc++ -fshort-wchar -fms-extensions -Wno-everything
    -include $root/platform/include/port/compat.h)
export CPATH=${CPATH:-/home/claude/g3/desk/shim}
nice -n 10 clang++ "${FL[@]}" -c "$root/platform/src/os/os_mem.cpp" -o "$work/os_mem.o"
nice -n 10 clang++ "${FL[@]}" -c "$root/tests/mp/vita_limits_probe.cpp" -o "$work/probe.o"
P() { clang++ -print-file-name="$1"; }
clang++ -stdlib=libc++ -fuse-ld=lld -no-pie -nostartfiles -Wl,--image-base=0x98000000 "$(P crt1.o)" "$(P crti.o)" "$(P crtbeginS.o)" \
  "$work/probe.o" "$work/os_mem.o" "$(P crtendS.o)" "$(P crtn.o)" -o "$work/probe" -ldl -lpthread
run() { env GLIBC_TUNABLES=glibc.malloc.mmap_threshold=0 "$@" "$work/probe" | grep -o 'MEM1 0x[0-9a-f]*+[0-9a-f]* MEM2 0x[0-9a-f]*+[0-9a-f]*' | sed 's/0x[0-9a-f]*+/+/g'; }
fail=0
check() { local want=$1 got; shift; got=$(run "$@"); if [ "$got" = "$want" ]; then echo "ok   $*: $got"; else echo "FAIL $*: got '$got' want '$want'"; fail=1; fi; }
for native in 0 1; do
  check "MEM1 +8000000 MEM2 +8000000" PETARI_NATIVE_ADDRESSES=$native
  check "MEM1 +8000000 MEM2 +8000000" PETARI_NATIVE_ADDRESSES=$native PETARI_VITA_LIMITS=0
  check "MEM1 +1800000 MEM2 +4000000" PETARI_NATIVE_ADDRESSES=$native PETARI_VITA_LIMITS=1
done
check "MEM1 +1800000 MEM2 +6000000" PETARI_NATIVE_ADDRESSES=1 PETARI_VITA_LIMITS=1 PETARI_MEM2_MB=96
check "MEM1 +1800000 MEM2 +4000000" PETARI_NATIVE_ADDRESSES=1 PETARI_VITA_LIMITS=1 PETARI_MEM2_MB=32
check "MEM1 +8000000 MEM2 +8000000" PETARI_NATIVE_ADDRESSES=1 PETARI_MEM2_MB=96
[ $fail = 0 ] && echo "vita_limits_test: PASS"
exit $fail
