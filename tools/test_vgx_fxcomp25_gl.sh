#!/usr/bin/env bash
# compositor25: GXV_FXLOWRES_AUTO pictures on Mesa (tests/vgx_fxcomp25_gl_test.cpp) and their comparison (tools/fxcomp25_compare.py).
#   tools/test_vgx_fxcomp25_gl.sh [output-dir]      (KHR_INCLUDE default /work/khr/include; SAN=1 for ASan/UBSan)
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
src="$root/platform/vgx/src"
khr=${KHR_INCLUDE:-/work/khr/include}
san=()
[[ "${SAN:-0}" == 1 ]] && san=(-fsanitize=address,undefined -fno-omit-frame-pointer)
nice -n 10 "${CXX:-g++}" -std=gnu++20 -O1 -g "${san[@]}" -DPORT_RUNTIME_DIAGNOSTICS=0 -DPORT_LOGGING=0 \
  -I"$src" -I"$root/platform/vgx/third_party" -I"$root/platform/include" -I"$khr" \
  "$root/tests/vgx_fxcomp25_gl_test.cpp" "$src/vgx_backend_gl.cpp" "$src/vgx_autoq.cpp" "$src/vgx_shadergen.cpp" \
  "$root/platform/vgx/host/vgx_host_egl.cpp" -lEGL -lGLESv2 -lpthread -o "$out/vgx_fxcomp25_gl_test"
env -i PATH="$PATH" HOME="${HOME:-/tmp}" LIBGL_ALWAYS_SOFTWARE=1 LP_NUM_THREADS=1 ASAN_OPTIONS=detect_leaks=0 \
  GXV_LOG=0 GXV_FXHALF=0 GXV_WORKLOAD_PROBE=0 GXV_AUTOQ=0 GXV_KEEP_RT=0 GXV_FXLOWRES_AUTO_EVERY=1 GXV_FXLOWRES_EVERY=1 \
  "$out/vgx_fxcomp25_gl_test" "$out/p" > "$out/run.log" 2>&1 || { echo "FAILED"; cat "$out/run.log"; exit 1; }
cat "$out/run.log"
# round 3: the console's configuration: entry.cpp's defaults for every GXV_FXLOWRES* name and GXV_FXLOWRES_AUTO=1 alone (no EVERY=1, no stage knobs, default window)
env -i PATH="$PATH" HOME="${HOME:-/tmp}" LIBGL_ALWAYS_SOFTWARE=1 LP_NUM_THREADS=1 ASAN_OPTIONS=detect_leaks=0 GXV_AUTOQ=0 GXV_KEEP_RT=0 \
  "$out/vgx_fxcomp25_gl_test" "$out/c" console > "$out/console.log" 2>&1 || { echo "FAILED (console configuration)"; cat "$out/console.log"; exit 1; }
grep -a "^RESULT\|^STAGE\|^LOG\|^PASS\|^FAIL" "$out/console.log"
grep -q -E "runtime error|AddressSanitizer" "$out/run.log" && { echo "FAILED (sanitizer)"; exit 1; }
python3 -I "$root/tools/fxcomp25_compare.py" "$out/p"
