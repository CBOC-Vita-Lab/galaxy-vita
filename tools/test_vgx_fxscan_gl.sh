#!/usr/bin/env bash
# Real indexed draws on Mesa. Separate processes compare exact readbacks and
# the ordered GL operations/arguments, not timings or diagnostic counters.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
src="$root/platform/vgx/src"
wraps=(glUseProgram glEnable glDisable glViewport glScissor glClear glClearColor glClearDepthf
  glColorMask glDepthMask glDepthFunc glBlendFunc glBlendEquation glActiveTexture glBindTexture
  glTexParameteri glUniform1i glUniform1f glUniform2f glUniform4f glFramebufferTexture2D glFlush
  glFramebufferRenderbuffer glRenderbufferStorage glTexImage2D glCopyTexImage2D
  glDrawElements glDrawArrays glUniform4fv glUniformMatrix4fv glBindFramebuffer glCopyTexSubImage2D
  glReadPixels glFinish)
link=()
for symbol in "${wraps[@]}"; do link+=("-Wl,--wrap=$symbol"); done
nice -n 10 "${CXX:-g++}" -std=gnu++20 -O1 -g -DPORT_RUNTIME_DIAGNOSTICS=0 -DPORT_LOGGING=0 \
  -I"$src" -I"$root/platform/vgx/third_party" \
  "$root/tests/vgx_fxscan_gl_test.cpp" "$src/vgx_backend_gl.cpp" "$src/vgx_autoq.cpp" "$src/vgx_shadergen.cpp" \
  "$root/platform/vgx/host/vgx_host_egl.cpp" "${link[@]}" -lEGL -lGLESv2 -lpthread -o "$out/vgx_fxscan_gl_test"
for fx in 0 2 4; do
  for mode in unset 0 1 2; do
    setting=()
    if [[ "$mode" != unset ]]; then setting+=("GXV_FXSCANFAST=$mode"); fi
    env -i PATH="$PATH" HOME="${HOME:-/tmp}" LIBGL_ALWAYS_SOFTWARE=1 LP_NUM_THREADS=1 \
      GXV_LOG=0 GXV_FXHALF="$fx" GXV_FXHALF_MIN=0 GXV_WORKLOAD_PROBE=0 GXV_AUTOQ=0 GXV_KEEP_RT=0 \
      "${setting[@]}" "$out/vgx_fxscan_gl_test" "$out/fx-$fx-mode-$mode" > "$out/fx-$fx-mode-$mode.log" 2>&1
    cat "$out/fx-$fx-mode-$mode.log"
  done
  for mode in unset 1 2; do
    cmp "$out/fx-$fx-mode-0.trace" "$out/fx-$fx-mode-$mode.trace"
    cmp "$out/fx-$fx-mode-0.rgba" "$out/fx-$fx-mode-$mode.rgba"
  done
  printf 'PASS: FXHALF=%s modes unset/0/1/2 have byte-identical ordered GL traces and RGBA readbacks\n' "$fx"
done
