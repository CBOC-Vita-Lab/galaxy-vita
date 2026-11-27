#!/bin/bash
# Host test of the profile writer: instrument a tiny program, write its raw profile with the Vita build's writer sources, then let the
# matching llvm-profdata merge and show it, and compile the program again with -fprofile-use (no "out of date" warning, counts applied).
#   tests/pgo_host/run_test.sh <work-dir>      (needs clang 18 and llvm-profdata 18; the profile writer is the 18.1.3 one)
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd); root=$(cd "$here/../.." && pwd); W=${1:?work dir}; mkdir -p "$W"; cd "$W"
rt=$root/vita/pgo/compiler-rt; pgo=$root/vita/pgo
INC="-I$rt/include -I$rt/profile -I$pgo -DCOMPILER_RT_HAS_ATOMICS=1 -fvisibility=hidden -DCOMPILER_RT_HAS_UNAME=0"
objs=()
for f in "$rt"/profile/InstrProfiling{,Buffer,Internal,Value,Writer}.c "$pgo/vita_pgo_core.c" "$pgo/vita_pgo_platform.c"; do
  clang -O2 $INC -c "$f" -o "$(basename "$f").o"; objs+=("$(basename "$f").o")
done
# vita_pgo_platform.c defines the Vita's runtime hook; the Linux driver does not reference it: harmless here
clang -O2 -fprofile-generate -I"$pgo" -c "$here/pgo_writer_host.c" -o prog.o
clang prog.o "${objs[@]}" -o prog          # (no -fprofile-generate at the link: the stock runtime stays out, the writer is ours)
./prog prog.profraw
llvm-profdata merge -o prog.profdata prog.profraw
llvm-profdata show --all-functions --counts prog.profdata | tee show.txt
echo "--- -fprofile-use:"
clang -O2 -fprofile-use=prog.profdata -Wprofile-instr-out-of-date -Wprofile-instr-missing -I"$pgo" -S -emit-llvm "$here/pgo_writer_host.c" -o use.ll 2>use.warn || true
echo "warnings: $(grep -c warning use.warn || true)"; grep -c function_entry_count use.ll | sed 's/^/functions with counts: /'
