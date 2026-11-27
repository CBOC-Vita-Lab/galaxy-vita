#!/bin/bash
# The same test as run_test.sh as a 32-bit (ILP32, little endian) program, like the Vita: -m32 freestanding with a tiny C library of its own
# (the host has no 32-bit glibc).  The raw profile it writes is the 32-bit format (pointer size 4, magic "\377lprofr\201" 32-bit variant).
#   tests/pgo_host/run_test32.sh <work-dir>
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd); root=$(cd "$here/../.." && pwd); W=${1:?work dir}; mkdir -p "$W"; cd "$W"
rt=$root/vita/pgo/compiler-rt; pgo=$root/vita/pgo
CF="-m32 -ffreestanding -fno-builtin -fno-stack-protector -nostdinc -isystem $(clang -print-resource-dir)/include -I$here/shim32 -I$rt/include -I$rt/profile -I$pgo"
CF="$CF -DCOMPILER_RT_HAS_ATOMICS=1 -fvisibility=hidden -DPGO_I386_FREESTANDING=1"
objs=()
for f in "$rt"/profile/InstrProfiling{,Buffer,Internal,Value,Writer}.c "$pgo/vita_pgo_core.c" "$pgo/vita_pgo_platform.c" "$here/i386_libc.c"; do
  clang -O2 $CF -c "$f" -o "$(basename "$f").o32"; objs+=("$(basename "$f").o32")
done
clang -O2 -fprofile-generate $CF -c "$here/pgo_writer_host.c" -o prog.o32
clang -m32 -nostdlib -static -fuse-ld=lld prog.o32 "${objs[@]}" -o prog32
./prog32 prog32.profraw
file prog32 | cut -c1-80; od -A d -t x1 -N 16 prog32.profraw
llvm-profdata merge -o prog32.profdata prog32.profraw
llvm-profdata show --all-functions --counts --ic-targets prog32.profdata | tee show32.txt
