#!/usr/bin/env bash
# GXV_SANITIZE through the real front end and GLES backend on Mesa (software), no game data.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
src="$root/platform/vgx/src"
sources=(vgx_state vgx_draw vgx_fesplit vgx_dlcache vgx_guestmem vgx_bpsync
         vgx_shadergen vgx_texcache vgx_texdecode vgx_stats vgx_backend_gl vgx_autoq)
args=()
for unit in "${sources[@]}"; do args+=("$src/$unit.cpp"); done
"${CXX:-g++}" -std=c++20 -O0 -I"$src" -I"$root/platform/vgx/third_party" \
  "$root/tests/vgx_sanitize_gl.cpp" "${args[@]}" "$root/platform/vgx/host/vgx_host_egl.cpp" \
  -lEGL -lGLESv2 -pthread -o "$out/vgx_sanitize_gl"
run() { # <mode|unset> <scenario>
  local mode=$1 scenario=$2
  local flag=(-u GXV_SANITIZE)
  [[ "$mode" != unset ]] && flag=("GXV_SANITIZE=$mode")
  env "${flag[@]}" LIBGL_ALWAYS_SOFTWARE=1 LP_NUM_THREADS=1 GXV_LOG=0 GXV_SPLIT=0 GXV_AUTOQ=0 \
    GXV_FXHALF=0 GXV_PASSHALF=0 GXV_CROWD=0 GXV_EFBSWAP=0 \
    "$out/vgx_sanitize_gl" "$scenario" > "$out/$scenario.$mode.log" 2>&1 || { cat "$out/$scenario.$mode.log"; echo "FAIL: mode=$mode scenario=$scenario"; exit 1; }
}
cases=0
# Valid input: the same picture and no findings in every mode.
for mode in unset 0 1 2 3 4 7 15 16 23 31; do run "$mode" valid; cases=$((cases + 1)); done
for mode in 0 1 2 3 4 7 15 16 23 31; do cmp <(grep '^centre' "$out/valid.unset.log") <(grep '^centre' "$out/valid.$mode.log"); done
# Degenerate input: off keeps the old behaviour (no findings), on skips, log-only counts and skips nothing.
for scenario in nan-proj viewport copy; do
  for mode in unset 0 1 3 4 5 15; do run "$mode" "$scenario"; cases=$((cases + 1)); done
  cmp <(grep '^centre\|^texture' "$out/$scenario.unset.log") <(grep '^centre\|^texture' "$out/$scenario.0.log")
  cmp <(grep '^centre\|^texture' "$out/$scenario.unset.log") <(grep '^centre\|^texture' "$out/$scenario.4.log")
done
# Feedback (bit 4): a copy that reads its own destination is counted with bit 4 and never skipped; the picture is the same in every mode.
for mode in unset 0 3 4 16 23 31; do run "$mode" feedback-copy; cases=$((cases + 1)); done
for mode in 0 3 4 16 23 31; do cmp <(grep '^texture' "$out/feedback-copy.unset.log") <(grep '^texture' "$out/feedback-copy.$mode.log"); done
printf 'PASS: %s GXV_SANITIZE cases (host GLES, software); unset/0 and log-only leave the picture as before, valid input is identical in every mode\n' "$cases"
