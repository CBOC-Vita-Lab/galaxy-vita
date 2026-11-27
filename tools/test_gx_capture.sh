#!/usr/bin/env bash
# GX capture and replay host proof (platform/src/gx/gx_capture.cpp; tests/gx_capture.cpp says what each check is): the production GX
# units and the layer, direct against captured + replayed, in every mode:
#   mode 1 (seeds, replays on this thread), mode 1 with GXV_GXCAPTURE_SYNC=1, mode 2 (verify: no difference may be reported), the
#   self-test (GXV_GXCAPTURE_SELFTEST=1, with and without a call cap), the negative control, then AddressSanitizer + UndefinedBehavior-
#   Sanitizer builds of modes 1 and 2 and a ThreadSanitizer build with the replay on a second thread (GXCAP_THREAD=1).
#   tools/test_gx_capture.sh [seeds]        (build in $OUT, default a temporary directory, removed at the end)
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
seeds=${1:-60}
out=${OUT:-$(mktemp -d)}
[[ -n "${OUT:-}" ]] || trap 'rm -rf "$out"' EXIT
mkdir -p "$out"
CC=${CC:-clang}; CXX=${CXX:-clang++}
inc=(-I"$root/platform/include" -I"$root/decomp/include" -I"$root/decomp/libs/RVL_SDK/include" -I"$root/decomp/libs/JSystem/include")
cflags=(-std=gnu11 -fno-strict-aliasing -fms-extensions -DTARGET_PC=1 "${inc[@]}" -include "$root/platform/include/port/compat.h" -w
        -Wno-implicit-function-declaration)
cxxflags=(-std=c++17 -fno-strict-aliasing -fms-extensions -DTARGET_PC=1 "${inc[@]}" -include string -include vector
          -include "$root/platform/include/port/compat.h" -w -pthread)
build() { # build <name> <extra flags...>
  local name=$1; shift
  local o="$out/$name"; mkdir -p "$o"
  local objs=()
  for u in GXAttr GXBump GXDisplayList GXFrameBuf GXGeometry GXInit GXLight GXMisc GXPixel GXTev GXTexture GXTransform; do
    "$CC" "${cflags[@]}" "$@" -c "$root/decomp/src/RVL_SDK/gx/$u.c" -o "$o/$u.o"; objs+=("$o/$u.o")
  done
  "$CC" "${cflags[@]}" "$@" -c "$root/platform/src/sdk/mtx.c" -o "$o/mtx.o"
  "$CC" "${cflags[@]}" "$@" -c "$root/tests/gx_capture_stubs.c" -o "$o/stubs.o"
  "$CXX" "${cxxflags[@]}" "$@" -c "$root/platform/src/gx/gx_capture.cpp" -o "$o/gx_capture.o"
  "$CXX" "${cxxflags[@]}" "$@" -c "$root/tests/gx_capture.cpp" -o "$o/test.o"
  "${LINK:-$CXX}" "$@" -pthread "$o/test.o" "$o/gx_capture.o" "$o/stubs.o" "$o/mtx.o" "${objs[@]}" -Wl,--allow-multiple-definition -o "$out/$name.bin"
}
fail=0
run() { # run <label> <binary> [env...]
  local label=$1 bin=$2; shift 2
  if env "$@" "$bin" "$seeds" > "$out/$label.log" 2>&1; then echo "PASS $label: $(grep -E '^checks' "$out/$label.log" | cut -c1-160)"
  else echo "FAIL $label"; tail -20 "$out/$label.log"; fail=1; fi
  grep -E '^abandons by' "$out/$label.log" | cut -c1-200 | sed 's/^/     /' || true
  grep -E '^cheap path' "$out/$label.log" | sed 's/^/     /' || true
}
build plain -O2 -g
run mode1 "$out/plain.bin" GXV_GXCAPTURE=1
run mode1-sync "$out/plain.bin" GXV_GXCAPTURE=1 GXV_GXCAPTURE_SYNC=1
run mode2-verify "$out/plain.bin" GXV_GXCAPTURE=2
run mode2-verify-sync "$out/plain.bin" GXV_GXCAPTURE=2 GXV_GXCAPTURE_SYNC=1
run selftest "$out/plain.bin" GXV_GXCAPTURE=1 GXV_GXCAPTURE_SELFTEST=1
run selftest-cap "$out/plain.bin" GXV_GXCAPTURE=1 GXV_GXCAPTURE_SELFTEST=1 GXV_GXCAPTURE_SELFTEST_CALLS=7
run selftest-verify "$out/plain.bin" GXV_GXCAPTURE=2 GXV_GXCAPTURE_SELFTEST=1 GXV_GXCAPTURE_SELFTEST_CALLS=13
run negative "$out/plain.bin" GXV_GXCAPTURE=1 GXCAP_NEG=1
# (the record path of before the cheap one, kept behind a switch: it must stay right as long as the switch exists)
run mode1-slow "$out/plain.bin" GXV_GXCAPTURE=1 GXV_GXCAPTURE_SLOW=1
run selftest-cap-slow "$out/plain.bin" GXV_GXCAPTURE=1 GXV_GXCAPTURE_SLOW=1 GXV_GXCAPTURE_SELFTEST=1 GXV_GXCAPTURE_SELFTEST_CALLS=7
run null-recorder "$out/plain.bin" GXV_GXCAPTURE=1 GXCAP_NULLTEST=1
run owner-window "$out/plain.bin" GXV_GXCAPTURE=1 GXCAP_OWNERTEST=1
run replay-trap "$out/plain.bin" GXV_GXCAPTURE=1 GXV_GXCAPTURE_SYNC=1 GXCAP_TRAPTEST=1
if [[ "${SAN:-1}" == 1 ]]; then
  # (this host's clang has no sanitizer runtimes: the sanitizer builds compile with clang and link with g++, whose libasan / libubsan /
  # libtsan take clang's instrumentation; LINK_CXX overrides)
  LINK=${LINK_CXX:-g++}
  build asan -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined
  run asan-mode1 "$out/asan.bin" GXV_GXCAPTURE=1 ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
  run asan-mode2 "$out/asan.bin" GXV_GXCAPTURE=2 ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
  run asan-selftest "$out/asan.bin" GXV_GXCAPTURE=1 GXV_GXCAPTURE_SELFTEST=1 GXV_GXCAPTURE_SELFTEST_CALLS=5 ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1
  run asan-thread "$out/asan.bin" GXV_GXCAPTURE=1 GXCAP_THREAD=1 ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1
  run asan-mode1-slow "$out/asan.bin" GXV_GXCAPTURE=1 GXV_GXCAPTURE_SLOW=1 ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1
  run asan-replay-trap "$out/asan.bin" GXV_GXCAPTURE=1 GXV_GXCAPTURE_SYNC=1 GXCAP_TRAPTEST=1 ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1
  run asan-null-recorder "$out/asan.bin" GXV_GXCAPTURE=1 GXCAP_NULLTEST=1 ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1
  build tsan -O1 -g -fsanitize=thread
  run tsan-thread "$out/tsan.bin" GXV_GXCAPTURE=1 GXCAP_THREAD=1 TSAN_OPTIONS=halt_on_error=1
  run tsan-owner-window "$out/tsan.bin" GXV_GXCAPTURE=1 GXCAP_OWNERTEST=1 TSAN_OPTIONS=halt_on_error=1
  run tsan-thread-selftest "$out/tsan.bin" GXV_GXCAPTURE=1 GXCAP_THREAD=1 GXV_GXCAPTURE_SELFTEST=1 TSAN_OPTIONS=halt_on_error=1
fi
[[ $fail == 0 ]] && echo "ALL PASS" || { echo "SOME FAILED (logs in $out)"; exit 1; }
