#!/bin/bash
# copy49 (22 Nov 2026): host test of platform/include/port/smallcopy.h and vita/vgx-game/smallcopy_wrap.cpp (GXV_SMALLCOPY), on x86 with the
# portable vector code, under AddressSanitizer + UBSan, at -O1 and -O3; then an ARM compile check of the NEON path with the console's clang flags
# (and arm-vita-eabi-gcc -fsyntax-only), and a scan of the ARM object for the instructions that must never be formed (LDRD / LDM / STM / STRD
# inside the three wrappers) and for any call to memcpy / memmove / memset in them.
set -euo pipefail
cd "$(dirname "$0")/.."
out=${1:-/tmp/copy49-test}; mkdir -p "$out"
for opt in -O1 -O3; do
  # the wrapper unit is compiled -fno-builtin (as on the console); the test unit is not
  g++ -std=gnu++17 $opt -fno-builtin -fno-tree-loop-distribute-patterns -fsanitize=address,undefined -fno-sanitize-recover=undefined -Iplatform/include -Itests/copy49_stub -c vita/vgx-game/smallcopy_wrap.cpp -o "$out/wrap$opt.o"
  g++ -std=gnu++17 $opt -fsanitize=address,undefined -fno-sanitize-recover=undefined -Iplatform/include -Itests/copy49_stub -c tests/copy49_smallcopy_test.cpp -o "$out/test$opt.o"
  g++ -fsanitize=address,undefined -pthread "$out/wrap$opt.o" "$out/test$opt.o" -o "$out/t$opt"
  echo "== x86 $opt"; "$out/t$opt"
  echo "== x86 $opt, sc55 registry (cached-memory-only rule) + fx59 (failures, self-test, alloc clear, race, 2-way)"; "$out/t$opt" registry
  echo "== x86 $opt, fx59 switch off: no clears, no lookups"; "$out/t$opt" off
  echo "== x86 $opt, fx59 failing self-test disables the inline path"; "$out/t$opt" selftestfail
  echo "== x86 $opt, sc70 census never asks newlib for a _reent (first-call recursion)"; "$out/t$opt" censusreent
done
V=${VITASDK:-/opt/vitasdk}/arm-vita-eabi; G=15.2.0
FL="--target=arm-vita-eabi --sysroot=$V -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -mthumb -fsigned-char -fno-short-enums -femulated-tls -fno-pic -munaligned-access -D__vita__ -nostdinc++ -nostdlibinc -isystem $V/include/c++/$G -isystem $V/include/c++/$G/arm-vita-eabi -isystem $V/include/c++/$G/backward -isystem $V/include -std=gnu++17 -fno-exceptions -fno-rtti -fno-builtin -Iplatform/include"
for opt in -O2 -O3; do
  clang++ $FL $opt -Wall -Wextra -c vita/vgx-game/smallcopy_wrap.cpp -o "$out/arm$opt.o"
  llvm-objdump -dr --no-show-raw-insn "$out/arm$opt.o" | awk '/^[0-9a-f]+ <__wrap_/{p=1} /^[0-9a-f]+ <(vgxvita|_GLOBAL|__wrap_sceKernel)/{p=0} p' > "$out/arm$opt.wrap.s"
  if grep -E "\b(ldrd|strd|ldm|stm|ldmia|stmia|stmdb|ldmdb|vldm|vstm)" "$out/arm$opt.wrap.s" | grep -vE "vpush|vpop|\[sp[],]"; then echo "FAIL: forbidden multi-word access in the ARM $opt wrappers"; exit 1; fi
  if grep -E "R_ARM_THM_(CALL|JUMP24)\s+(memcpy|memmove|memset|sceClibMemcpy)\b" "$out/arm$opt.wrap.s"; then echo "FAIL: a wrapper calls the function it stands in for"; exit 1; fi
  echo "== ARM clang $opt: compiled; no LDRD/STRD/LDM/STM, no call to memcpy/memmove/memset in the wrappers; vector accesses: $(grep -cE 'vld1\.8|vst1\.8' "$out/arm$opt.wrap.s")"
done
if [ -x "${VITASDK:-/opt/vitasdk}/bin/arm-vita-eabi-g++" ]; then
  "${VITASDK:-/opt/vitasdk}/bin/arm-vita-eabi-g++" -std=gnu++17 -fsyntax-only -O2 -fno-builtin -fno-tree-loop-distribute-patterns -Iplatform/include -x c++ - <<<'#include "port/smallcopy.h"
void f(void*d,const void*s,unsigned n){portsc::Copy(d,s,n);portsc::Fill(d,1,n);}'
  echo "== arm-vita-eabi-g++ -fsyntax-only: ok"
fi
