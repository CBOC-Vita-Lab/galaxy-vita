#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
src="$root/platform/vgx/src"
"${CXX:-c++}" -std=c++20 -O1 -g -Wall -Wextra -fsanitize=address,undefined \
  -I"$src" -I"$root/platform/vgx/third_party" \
  "$root/tests/vgx_tevwrap_elide.cpp" "$src/vgx_shadergen.cpp" -o "$out/vgx_tevwrap_elide"
for wrap in 0 1; do
  wrapping=off; [[ $wrap != 1 ]] || wrapping=on
  for value in unset 0 1 2 3 -1 invalid 4294967297 4294967298; do
    mode=off; [[ $value != 1 && $value != 2 ]] || mode=on
    if [[ $value == unset ]]; then
      env -u GXV_TEVWRAP_ELIDE GXV_TEVWRAP="$wrap" "$out/vgx_tevwrap_elide" "$mode" "$wrapping"
    else
      GXV_TEVWRAP_ELIDE="$value" GXV_TEVWRAP="$wrap" "$out/vgx_tevwrap_elide" "$mode" "$wrapping"
    fi
  done
done
# Compile a mechanical forward-proof fault, without adding a production test hook.
sed -e 's/color = bounded(st.color, true);/color = true;/' \
    -e 's/alpha = bounded(st.alpha, false);/alpha = true;/' \
    "$src/vgx_shadergen.cpp" > "$out/fault.cpp"
"${CXX:-c++}" -std=c++20 -O1 -g -Wall -Wextra -Wno-unused-variable -fsanitize=address,undefined \
  -I"$src" -I"$root/platform/vgx/third_party" \
  "$root/tests/vgx_tevwrap_elide.cpp" "$out/fault.cpp" -o "$out/vgx_tevwrap_elide_fault"
if GXV_TEVWRAP=1 GXV_TEVWRAP_ELIDE=1 "$out/vgx_tevwrap_elide_fault" on on > "$out/fault-unverified.log" 2>&1; then
  echo 'FAIL: injected forward-proof fault was not detected' >&2; exit 1
fi
grep -q 'FAIL: RGB and alpha bounds stay independent' "$out/fault-unverified.log"
GXV_TEVWRAP=1 GXV_TEVWRAP_ELIDE=2 "$out/vgx_tevwrap_elide_fault" on on > "$out/fault-verified.log" 2>&1
[[ $(grep -c 'verifier disagreement' "$out/fault-verified.log") -eq 1 ]]
echo 'PASS: injected forward-proof fault is caught; verified mode preserves required wrappers and logs once'
