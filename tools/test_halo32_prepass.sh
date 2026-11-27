#!/usr/bin/env bash
# halo32: the HUD reader's depth pre-pass drawn in Mesa GLES against the game's own order (tests/halo32_prepass_pixels.cpp), then once per fault: each must FAIL.
# Needs the GLES2/EGL/KHR headers (CPATH=<dir with GLES2 EGL KHR>) and libEGL / libGLESv2 (Mesa software: EGL_PLATFORM=surfaceless).
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=$(mktemp -d "${TMPDIR:-/tmp}/halo32.XXXXXX"); trap 'rm -rf "$out"' EXIT
CXX=${CXX:-c++}
libs=(${HALO32_LIBS:--lEGL -lGLESv2})
fail=0
"$CXX" -std=c++17 -O1 "$root/tests/halo32_prepass_pixels.cpp" "${libs[@]}" -o "$out/t0"
EGL_PLATFORM=surfaceless LIBGL_ALWAYS_SOFTWARE=1 "$out/t0" || fail=1
for m in 1 2 3; do
    "$CXX" -std=c++17 -O1 -DHALO32_MUTANT=$m "$root/tests/halo32_prepass_pixels.cpp" "${libs[@]}" -o "$out/t$m"
    if EGL_PLATFORM=surfaceless LIBGL_ALWAYS_SOFTWARE=1 "$out/t$m" > "$out/m$m.log" 2>&1; then echo "mutant $m PASSED: the test cannot see that fault"; fail=1
    else echo "== mutant $m fails, as it must: $(head -1 "$out/m$m.log")"; fi
done
[[ $fail -eq 0 ]] || { echo "halo32 prepass: FAIL"; exit 1; }
echo "halo32 prepass: PASS"
