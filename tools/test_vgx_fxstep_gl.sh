#!/usr/bin/env bash
# The quality steps (GXV_AUTOQ_FXSTEP) on the real GLES back end and Mesa. Separate processes compare exact readbacks and the
# ordered GL operations. No game data. Needs the Mesa EGL/GLES host libraries (as tools/test_vgx_fxscan_gl.sh).
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
  "$root/tests/vgx_fxstep_gl_test.cpp" "$src/vgx_backend_gl.cpp" "$src/vgx_autoq.cpp" "$src/vgx_shadergen.cpp" \
  "$root/platform/vgx/host/vgx_host_egl.cpp" "${link[@]}" -lEGL -lGLESv2 -lpthread -o "$out/vgx_fxstep_gl_test"
run() { # <name> <fx|bg> <schedule> [env assignment...]
  local name=$1 what=$2 schedule=$3; shift 3
  env -i PATH="$PATH" HOME="${HOME:-/tmp}" LIBGL_ALWAYS_SOFTWARE=1 LP_NUM_THREADS=1 \
    GXV_LOG=0 GXV_FXHALF_MIN=0 GXV_WORKLOAD_PROBE=0 GXV_AUTOQ=0 GXV_KEEP_RT=0 "$@" \
    "$out/vgx_fxstep_gl_test" "$what" "$out/$name" "$schedule" > "$out/$name.log" 2>&1 || { cat "$out/$name.log"; exit 1; }
  grep -E '^(frame|PASS|FAIL)' "$out/$name.log"
}
after_marker() { # <trace> : the ordered operations from the first scheduled frame on, GL program names numbered by first use
  python3 - "$1" <<'PY'
import struct, sys
d = open(sys.argv[1], 'rb').read()
i = d.index(b'first scheduled frame\0')
names = {}
while i < len(d):
    j = d.index(b'\0', i)
    op = d[i:j].decode()
    i = j + 1
    n = struct.unpack_from('<I', d, i)[0]
    i += 4
    args = list(struct.unpack_from('<%dQ' % n, d, i))
    i += 8 * n
    extra = b''
    if op == 'glUniform4fv':
        extra = d[i:i + 16 * args[1]]; i += 16 * args[1]
    elif op == 'glUniformMatrix4fv':
        extra = d[i:i + 64 * args[1]]; i += 64 * args[1]
    if op == 'glUseProgram':
        args[0] = names.setdefault(args[0], len(names))
    print(op, args, extra.hex())
PY
}
same_ops() { # <a> <b>
  after_marker "$out/$1.trace" > "$out/$1.ops"; after_marker "$out/$2.trace" > "$out/$2.ops"; cmp "$out/$1.ops" "$out/$2.ops"
}
same() { # <a> <b> <frames...> : identical per-frame readbacks
  local a=$1 b=$2; shift 2
  for f in "$@"; do cmp "$out/$a.f$f.rgba" "$out/$b.f$f.rgba"; done
}
# ---- half-size effects
run fx-base fx --- 
run fx-static fx --- GXV_FXHALF=2
run fx-all fx fff
run fx-off fx ---
# off is exact: stepping nothing equals a process that never heard of the steps
same_ops fx-base fx-off; same fx-base fx-off 2 3 4
# toggled on equals the static switch: the same ordered GL operations and the same pictures
same_ops fx-static fx-all; same fx-static fx-all 2 3 4
# between frames, on and off in any order: every frame equals the static (on) or the base (off) frame
# the same scene each frame, any order of steps (and the bits of the pictures: half-size is a different picture from full size)
run fxi-base fxi --- 
run fxi-static fxi --- GXV_FXHALF=2
run fxi-on-off-on fxi f-f
run fxi-off-on-off fxi -f-
run fxi-on-off-off fxi ff-
cmp -s "$out/fxi-base.f2.rgba" "$out/fxi-static.f2.rgba" && { echo "FAIL: the half-size picture is the full-size picture: the check below would prove nothing"; exit 1; }
same fxi-static fxi-on-off-on 2 4; same fxi-base fxi-on-off-on 3
same fxi-base fxi-off-on-off 2 4; same fxi-static fxi-off-on-off 3
same fxi-static fxi-on-off-off 2 3; same fxi-base fxi-on-off-off 4
printf 'PASS: half-size effects as a runtime step: off = base, on = GXV_FXHALF=2 (trace and pictures), toggled between frames = the static frame of each state\n'
# ---- cheaper background (only the mip filter of non-character textures)
run bg-base bg ---
run bg-all bg bbb
run bg-mid bg b-b
run bg-other bg -b-
same bg-base bg-mid 3; same bg-base bg-other 2 4; same bg-all bg-mid 2 4; same bg-all bg-other 3
printf 'PASS: cheaper background: off is the base picture, on takes the nearest mip level for the ordinary texture only, the character keeps its filter\n'
