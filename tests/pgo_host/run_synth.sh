#!/bin/bash
# Real units, real records: cut the profile sections out of a linked instrumented Vita ELF, put them in a 32-bit test program that writes them
# with the Vita build's writer, let llvm-profdata merge the file, and compile the real units again with -fprofile-use.
#   tests/pgo_host/run_synth.sh <work-dir> <instrumented galaxy_native ELF> <counter mode 0|1|2>
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd); root=$(cd "$here/../.." && pwd); W=${1:?work dir}; ELF=${2:?elf}; MODE=${3:-2}; mkdir -p "$W"; cd "$W"
OC=${OBJCOPY:-/opt/vitasdk/bin/arm-vita-eabi-objcopy}
for s in cnts data names; do $OC -O binary --only-section=__llvm_prf_$s "$ELF" $s.bin; done
# (no value-profile data in this test: the records' value-site counts are cleared, 48-byte records, NumValueSites at offset 36)
python3 - <<'PY'
d = bytearray(open("data.bin", "rb").read())
for o in range(0, len(d), 48): d[o + 36:o + 40] = bytes(4)
open("data.bin", "wb").write(d)
PY
ls -la cnts.bin data.bin names.bin
rt=$root/vita/pgo/compiler-rt; pgo=$root/vita/pgo
CF="-m32 -ffreestanding -fno-builtin -fno-stack-protector -nostdinc -isystem $(clang -print-resource-dir)/include -I$here/shim32 -I$rt/include -I$rt/profile -I$pgo -DCOMPILER_RT_HAS_ATOMICS=1 -fvisibility=hidden"
objs=()
for f in "$rt"/profile/InstrProfiling{,Buffer,Internal,Writer}.c "$pgo/vita_pgo_platform.c" "$here/i386_libc.c" "$here/synth_i386.c"; do
  clang -O2 $CF -c "$f" -o "$(basename "$f").o32"; objs+=("$(basename "$f").o32")
done
clang -m32 -c -I. "$here/i386_synth.S" -o synth.o32
# (Value.c is left out: nothing here instruments; its two entry points are referenced by nobody)
clang -m32 -nostdlib -static -fuse-ld=lld "${objs[@]}" synth.o32 -o synth32 2>&1 | head -5 || true
./synth32 synth.profraw $MODE
ls -la synth.profraw
llvm-profdata merge --sparse -o synth.profdata synth.profraw
llvm-profdata show synth.profdata | tail -5
