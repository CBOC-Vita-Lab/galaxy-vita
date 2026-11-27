#!/usr/bin/env bash
# stdio31: the host proofs of platform/include/port/console_stdio.h (the Vita diagnostic build's console streams).
#  1. The VitaSDK's own newlib stdio objects (from $VITASDK/arm-vita-eabi/lib/libc.a, unchanged, renamed nl_*) in an armhf program under
#     qemu-arm: several threads print to one buffered stream while a writer thread flushes it. "old" = the C library called directly (the
#     pattern before stdio31: expected to corrupt or crash), "safe" = through port::console::Sink, "stress" = safe plus the deadlock checks
#     (a printing alarm-like thread holding a "CPU" lock, the log writer's locks in os_alarm.cpp's order, a write hook that prints and logs,
#     slow writes, fflush(NULL), long lines, a reopen, the lock-order check and a watchdog).
#  2. The same program natively under ThreadSanitizer, the stream a model with no lock at all: any two threads in it at once is a reported race.
# Needs: qemu-arm-static, arm-linux-gnueabihf-g++, the VitaSDK (arm-vita-eabi-gcc, -ar, -objcopy), g++ with libtsan (TSAN_CXX=...; skipped if absent).
# Usage: tools/test_stdio31.sh [runs=10] [seconds=3]     Prints one line per run and a summary; exit 0 when every expectation holds.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
sdk=${VITASDK:-/opt/vitasdk}
runs=${1:-10}
secs=${2:-3}
out=${STDIO31_OUT:-$(mktemp -d /tmp/stdio31.XXXXXX)}
mkdir -p "$out/nl"
trap '[ -n "${STDIO31_KEEP:-}" ] || rm -rf "$out"' EXIT
qemu=$(command -v qemu-arm-static || command -v qemu-arm)

# ---- 1. newlib objects
members="fvwrite fflush fwrite fputs wbuf wsetup makebuf putc"
for m in $members; do (cd "$out/nl" && "$sdk/bin/arm-vita-eabi-ar" x "$sdk/arm-vita-eabi/lib/libc.a" "libc_a-$m.o"); done
"$sdk/bin/arm-vita-eabi-gcc" -O2 -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -mthumb -fno-builtin -c "$root/tests/stdio31/nl_shim.c" -o "$out/nl/shim.o"
syms="fflush _fflush_r __sflush_r fwrite _fwrite_r fputs _fputs_r __sfvwrite_r __swsetup_r __swbuf __swbuf_r __smakebuf_r __swhatbuf_r putc _putc_r
      __getreent __sinit _impure_data __sglue _malloc_r _free_r _realloc_r _fstat_r _isatty_r _fwalk_sglue"
: > "$out/nl/rename.txt"
for s in $syms; do echo "$s nl_$s" >> "$out/nl/rename.txt"; done
objs=()
for f in "$out"/nl/libc_a-*.o "$out"/nl/shim.o; do
  "$sdk/bin/arm-vita-eabi-objcopy" --redefine-syms="$out/nl/rename.txt" "$f" "${f%.o}.nl.o"
  objs+=("${f%.o}.nl.o")
done
echo "newlib members: $(for m in $members; do printf '%s ' "libc_a-$m.o"; done)(sha256 of libc.a: $(sha256sum "$sdk/arm-vita-eabi/lib/libc.a" | cut -c1-16))"
nice -n 10 arm-linux-gnueabihf-g++ -std=c++17 -O2 -DNL_BACKEND -I"$root/platform/include" -pthread -static \
  "$root/tests/stdio31/stdio_race.cpp" "${objs[@]}" -o "$out/race-nl" -Wl,--no-warn-mismatch 2> "$out/link.log" || { cat "$out/link.log"; exit 1; }

fail=0
summary=()
run_set() {  # mode buffering expectation(pass|fail) count
  local mode=$1 buf=$2 want=$3 n=$4 bad=0 ok=0 i st
  for ((i = 1; i <= n; i++)); do
    set +e
    line=$(timeout $((180 + ${secs%.*})) "$qemu" "$out/race-nl" "$mode" "$buf" "$secs" 4 2>&1 | tr '\n' ' ')
    st=${PIPESTATUS[0]}
    set -e
    if [ "$st" = 0 ]; then ok=$((ok + 1)); else bad=$((bad + 1)); fi
    echo "  [$mode $buf run $i] exit $st: ${line:0:420}"
  done
  summary+=("newlib $mode $buf: $bad of $n runs corrupted / crashed / stalled (expected: $([ "$want" = fail ] && echo 'corruption' || echo 'none'))")
  if [ "$want" = pass ] && [ "$bad" != 0 ]; then fail=1; fi
  if [ "$want" = fail ] && [ "$bad" = 0 ]; then echo "  (note: the old pattern did not show corruption in $n runs of $buf)"; fi
}
echo "== newlib under qemu-arm ($runs runs each, $secs s, 4 printing threads + the writer + an fflush(NULL) thread)"
run_set old fbf fail "$runs"
run_set old lbf fail "$runs"
run_set old nbf info 2
run_set safe fbf pass "$runs"
run_set safe lbf pass "$runs"
run_set safe nbf pass 2
run_set stress fbf pass "$runs"
run_set stress lbf pass 3
if [ -n "${STDIO31_SOAK:-}" ]; then  # (a long deadlock soak: STDIO31_SOAK=<seconds>)
  # (lines paced at 2 ms per writer, so the soak runs its whole length before the check buffer fills)
  set +e
  line=$(timeout $((300 + STDIO31_SOAK)) "$qemu" "$out/race-nl" stress fbf "$STDIO31_SOAK" 4 2000 2>&1 | tr '\n' ' ')
  st=${PIPESTATUS[0]}
  set -e
  echo "  [soak stress fbf ${STDIO31_SOAK} s] exit $st: ${line:0:420}"
  summary+=("newlib stress fbf soak ${STDIO31_SOAK} s: exit $st (expected 0)")
  [ "$st" = 0 ] || fail=1
fi

# ---- 2. TSan (native)
tsan_cxx=${TSAN_CXX:-g++}
if command -v "$tsan_cxx" > /dev/null; then
  echo "== ThreadSanitizer (native, the stream a model with no lock)"
  nice -n 10 "$tsan_cxx" -std=c++17 -O1 -g -fsanitize=thread -I"$root/platform/include" "$root/tests/stdio31/stdio_race.cpp" -o "$out/race-tsan" -pthread
  for mode in safe stress; do
    for buf in fbf lbf; do
      set +e
      TSAN_OPTIONS="halt_on_error=1 exitcode=66 report_signal_unsafe=0" "$out/race-tsan" $mode $buf "$secs" 4 > "$out/tsan.txt" 2>&1
      st=$?
      set -e
      echo "  [tsan $mode $buf] exit $st: $(grep -m1 -E "^(safe|stress)|WARNING: ThreadSanitizer|LOCK ORDER|DEADLOCK" "$out/tsan.txt" | cut -c1-300)"
      summary+=("tsan $mode $buf: exit $st")
      [ "$st" = 0 ] || { fail=1; sed -n 1,40p "$out/tsan.txt"; }
    done
  done
  # (control: the old pattern on the model must be reported by TSan, or the TSan part proves nothing)
  set +e
  TSAN_OPTIONS="halt_on_error=1 exitcode=66" "$out/race-tsan" old fbf 1 4 > "$out/tsan.txt" 2>&1
  st=$?
  set -e
  echo "  [tsan old fbf: control] exit $st (66 = TSan found the race): $(grep -m1 "WARNING: ThreadSanitizer" "$out/tsan.txt" || true)"
  summary+=("tsan old fbf (control, must be 66): exit $st")
  [ "$st" = 66 ] || fail=1
fi
# ---- 3. the Vita compilers on the changed units (-fsyntax-only), in each build flavour (player: diagnostics 0 / silent console; diagnostic: 1 / safe)
if [ -x "$sdk/bin/arm-vita-eabi-g++" ]; then
  echo "== Vita -fsyntax-only"
  gcc_flags=(-I"$root/platform/include" -I"$root/platform/src/gx" -I"$root/platform/vgx/src" -I"$root/vita/runtime" -O2 -std=gnu++20 -mcpu=cortex-a9
             -mfpu=neon -mfloat-abi=hard -mthumb -fsigned-char -fshort-wchar -fno-builtin-wcslen -fno-fast-math -Wall -Wextra -Wformat=2 -fsyntax-only)
  rt_flags=(--target=arm-vita-eabi --sysroot="$sdk/arm-vita-eabi" -DNDEBUG=1 -DTARGET_PC=1 -DVERSION=0 -I"$root/vita/include" -I"$root/platform/include"
            -I"$root/decomp/include" -I"$root/decomp/libs/JSystem/include" -I"$root/decomp/libs/RVL_SDK/include" -I"$root/decomp/libs/nw4r/include"
            -I"$root/decomp/libs/RVLFaceLib/include" -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -mthumb -fsigned-char -fno-short-enums -femulated-tls
            -fno-pic -munaligned-access -D__vita__ -D__USES_INITFINI__ -U_GNU_SOURCE -Wno-builtin-macro-redefined -U__WINT_TYPE__ "-D__WINT_TYPE__=unsigned int"
            -nostdlibinc -nostdinc++ -isystem "$sdk/arm-vita-eabi/include/c++/15.2.0" -isystem "$sdk/arm-vita-eabi/include/c++/15.2.0/arm-vita-eabi"
            -isystem "$sdk/arm-vita-eabi/include/c++/15.2.0/backward" -isystem "$sdk/arm-vita-eabi/include" -O2 -std=gnu++20 -fshort-wchar -fno-builtin-wcslen
            -fno-strict-aliasing -fwrapv -fms-extensions -include "$root/vita/include/vita_source_prelude.h" -include "$root/platform/include/port/compat.h"
            -Wno-register -Wno-multichar -Wno-invalid-offsetof -Wno-c++11-narrowing -Wno-writable-strings -Wno-deprecated-declarations -Wno-unknown-pragmas
            -Wno-ignored-attributes -Wno-microsoft-template -Wno-language-extension-token -Wformat -fno-exceptions -fno-rtti
            -DPORT_PLATFORM_CODE=1 -I"$root/vita/runtime" -DPORT_GX_VGX=1 -DPORT_GX_VGX_ONLY=1 -I"$root/platform/vgx/src" -fsyntax-only)
  entry_flags=()  # (entry.cpp as the game CMake compiles it: no extra warnings, -fno-short-enums)
  for f in "${gcc_flags[@]}"; do case $f in -Wall|-Wextra|-Wformat=2) ;; *) entry_flags+=("$f") ;; esac; done
  entry_flags+=(-fno-short-enums)
  syn() {  # label, command...
    local label=$1; shift
    if "$@" > "$out/syn.txt" 2>&1; then
      w=$(grep -c "warning:" "$out/syn.txt" || true); echo "  ok   $label ($w warnings)"; summary+=("syntax $label: ok")
      [ "$w" = 0 ] || grep "warning:" "$out/syn.txt" | sed -n 1,5p
    else echo "  FAIL $label"; sed -n 1,30p "$out/syn.txt"; summary+=("syntax $label: FAIL"); fail=1; fi
  }
  for d in 0 1; do
    syn "stdio_safe.cpp (diagnostics $d)" "$sdk/bin/arm-vita-eabi-g++" "${gcc_flags[@]}" -DPORT_RUNTIME_DIAGNOSTICS=$d -DPORT_LOGGING=1 "$root/vita/vgx-game/stdio_safe.cpp"
    syn "silent_stdio.cpp (wrap, diagnostics $d)" "$sdk/bin/arm-vita-eabi-g++" "${gcc_flags[@]}" -DPORT_SILENT_STDIO_WRAP=1 -DPORT_RUNTIME_DIAGNOSTICS=$d -DPORT_LOGGING=1 "$root/vita/vgx-game/silent_stdio.cpp"
    syn "entry.cpp (diagnostics $d)" "$sdk/bin/arm-vita-eabi-g++" "${entry_flags[@]}" -DPORT_RUNTIME_DIAGNOSTICS=$d -DPORT_LOGGING=1 "$root/vita/vgx-game/entry.cpp"
    if command -v clang++ > /dev/null; then
      for l in 0 1; do
        syn "os_alarm.cpp (diagnostics $d, logging $l)" clang++ "${rt_flags[@]}" -DPORT_RUNTIME_DIAGNOSTICS=$d -DPORT_LOGGING=$l "$root/platform/src/os/os_alarm.cpp"
      done
    fi
  done
  # the wraps of the game's link (vita/vgx-game/CMakeLists.txt) on a tiny program: it must link, and its console calls must all be __wrap_ calls
  wraps=(); for w in printf vprintf fprintf vfprintf puts putchar fputs fputc putc fwrite fflush; do wraps+=("-Wl,--wrap=$w"); done
  printf 'extern "C" void port_console_attach_writer(void){}\nextern "C" void port_console_drain(void){}\nextern "C" void port_console_configure(void){}\n' > "$out/stub.cpp"
  for flavour in safe silent; do
    if [ $flavour = safe ]; then src=("$root/vita/vgx-game/stdio_safe.cpp"); def=-DPORT_RUNTIME_DIAGNOSTICS=1
    else src=("$out/stub.cpp" "$root/vita/vgx-game/silent_stdio.cpp"); def="-DPORT_SILENT_STDIO_WRAP=1 -DPORT_RUNTIME_DIAGNOSTICS=0"; fi
    if "$sdk/bin/arm-vita-eabi-g++" -O2 -std=gnu++20 -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -mthumb -I"$root/platform/include" $def \
         "$root/tests/stdio31/vita_link_main.cpp" "${src[@]}" -o "$out/$flavour.elf" "${wraps[@]}" -pthread > "$out/syn.txt" 2>&1; then
      "$sdk/bin/arm-vita-eabi-objdump" -d --no-show-raw-insn "$out/$flavour.elf" > "$out/$flavour.dis"
      calls=$(awk '/<main>:/{f=1} f&&/^$/{exit} f' "$out/$flavour.dis" | grep -oE "<[a-z_]+>" | sort -u | tr '\n' ' ')
      unwrapped=$(echo "$calls" | tr ' ' '\n' | grep -E "^<(printf|vprintf|fprintf|vfprintf|puts|putchar|fputs|fputc|putc|fwrite|fflush)>$" || true)
      if [ -z "$unwrapped" ]; then echo "  ok   Vita link ($flavour): main calls $calls"; summary+=("vita link $flavour: ok")
      else echo "  FAIL Vita link ($flavour): unwrapped $unwrapped"; summary+=("vita link $flavour: FAIL"); fail=1; fi
    else echo "  FAIL Vita link ($flavour)"; sed -n 1,20p "$out/syn.txt"; summary+=("vita link $flavour: FAIL"); fail=1; fi
  done
fi
echo "== summary"
for s in "${summary[@]}"; do echo "  $s"; done
[ "$fail" = 0 ] && echo "stdio31 host proofs: PASS" || echo "stdio31 host proofs: FAIL"
exit "$fail"
