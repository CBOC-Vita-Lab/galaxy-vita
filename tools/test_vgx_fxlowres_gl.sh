#!/usr/bin/env bash
# GXV_FXLOWRES (lowres25): a reduced-size buffer for blended effects: the census (3), the path itself (1) and the diagnostic that leaves a
# class of draws out (GXV_FXLOWRES_CEILING). Real indexed draws through the production GLES back end on Mesa
# (tests/vgx_fxlowres_gl_test.cpp, tests/vgx_fxlowres_pic_gl_test.cpp). Separate processes: the switch unset, 0, 3 (census) and 2 (not
# built: counted as 3) must give byte-identical ordered GL traces and RGBA readbacks, i.e. the census draws, binds and allocates nothing; with it on the counters must hold the fixture's fill by class (far / near large-sprite / near /
# flat / sorted / not measured), also with other near limits and with one picture in four measured.
#   tools/test_vgx_fxlowres_gl.sh [output-dir]      (no game data, no Vita; Khronos headers: KHR_INCLUDE, default /work/khr/include)
#   SAN=1: build with -fsanitize=address,undefined
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
src="$root/platform/vgx/src"
khr=${KHR_INCLUDE:-/work/khr/include}
wraps=(glUseProgram glEnable glDisable glViewport glScissor glClear glClearColor glClearDepthf
  glColorMask glDepthMask glDepthFunc glBlendFunc glBlendEquation glActiveTexture glBindTexture
  glTexParameteri glUniform1i glUniform1f glUniform2f glUniform4f glFramebufferTexture2D glFlush
  glFramebufferRenderbuffer glRenderbufferStorage glTexImage2D glCopyTexImage2D
  glDrawElements glDrawArrays glUniform4fv glUniformMatrix4fv glBindFramebuffer glCopyTexSubImage2D
  glReadPixels glFinish)
link=()
for symbol in "${wraps[@]}"; do link+=("-Wl,--wrap=$symbol"); done
san=()
[[ "${SAN:-0}" == 1 ]] && san=(-fsanitize=address,undefined -fno-omit-frame-pointer)
nice -n 10 "${CXX:-g++}" -std=gnu++20 -O1 -g "${san[@]}" -DPORT_RUNTIME_DIAGNOSTICS=0 -DPORT_LOGGING=0 \
  -I"$src" -I"$root/platform/vgx/third_party" -I"$root/platform/include" -I"$khr" \
  "$root/tests/vgx_fxlowres_gl_test.cpp" "$src/vgx_backend_gl.cpp" "$src/vgx_autoq.cpp" "$src/vgx_shadergen.cpp" \
  "$root/platform/vgx/host/vgx_host_egl.cpp" "${link[@]}" -lEGL -lGLESv2 -lpthread -o "$out/vgx_fxlowres_gl_test"
run() { # <tag> env assignments...
  local tag=$1; shift
  env -i PATH="$PATH" HOME="${HOME:-/tmp}" LIBGL_ALWAYS_SOFTWARE=1 LP_NUM_THREADS=1 ASAN_OPTIONS=detect_leaks=0 \
    GXV_LOG=0 GXV_FXHALF=0 GXV_WORKLOAD_PROBE=0 GXV_AUTOQ=0 GXV_KEEP_RT=0 \
    "$@" "$out/vgx_fxlowres_gl_test" "$out/$tag" > "$out/$tag.log" 2>&1 || { echo "FAILED: $tag"; cat "$out/$tag.log"; exit 1; }
  cat "$out/$tag.log"
  grep -q -E "runtime error|AddressSanitizer" "$out/$tag.log" && { echo "FAILED (sanitizer): $tag"; exit 1; }
  return 0
}
run unset
run off GXV_FXLOWRES=0
run census GXV_FXLOWRES=3 GXV_FXLOWRES_EVERY=1
run census4 GXV_FXLOWRES=3
run line GXV_FXLOWRES=3 GXV_FXLOWRES_EVERY=1 GXV_FXLOWRES_LOG=3
run near6000 GXV_FXLOWRES=3 GXV_FXLOWRES_EVERY=1 GXV_FXLOWRES_NEAR=6000
run near0 GXV_FXLOWRES=3 GXV_FXLOWRES_EVERY=1 GXV_FXLOWRES_NEAR=0
run one GXV_FXLOWRES=1 GXV_FXLOWRES_EVERY=1
run two GXV_FXLOWRES=2 GXV_FXLOWRES_EVERY=1
# GXV_FXLOWRES_CEILING (the diagnostic that leaves a class of draws out): 2 finds no static candidate in this fixture and must change nothing;
# 1 / 3 / 4 must leave draws out (the program checks the counts and the pixels) and so change the trace.
run ceil1 GXV_FXLOWRES_CEILING=1
run ceil2 GXV_FXLOWRES_CEILING=2
run ceil3 GXV_FXLOWRES_CEILING=3 GXV_FXLOWRES=3 GXV_FXLOWRES_EVERY=1
run ceil4 GXV_FXLOWRES_CEILING=4
for tag in ceil1 ceil3 ceil4; do
  cmp -s "$out/unset.trace" "$out/$tag.trace" && { echo "FAILED: $tag left nothing out"; exit 1; }
  grep -q "THE PICTURE IS WRONG ON PURPOSE" "$out/$tag.log" || { echo "FAILED: $tag does not announce itself"; exit 1; }
done
cmp "$out/ceil1.trace" "$out/ceil3.trace" || { echo "FAILED: 1 and 3 (with the census on) must leave out the same draws here: no static candidate in the fixture"; exit 1; }
# (value 1 is the reduced-size path: it makes its depth-copy program, so its trace differs; this fixture's runs are far below the
# default gate of 1.5 screens, so nothing moves and the pixels are the same)
cmp "$out/unset.rgba" "$out/one.rgba"
for tag in off census census4 line near6000 near0 two ceil2; do
  cmp "$out/unset.trace" "$out/$tag.trace"
  cmp "$out/unset.rgba" "$out/$tag.rgba"
done
grep -q "far 0 big 0 near 0" "$out/unset.log" && grep -q "far 0 big 0 near 0" "$out/off.log" || { echo "FAILED: the census ran with the switch off"; exit 1; }
grep -q "^fxlowres: 3 pictures measured" "$out/line.log" || { echo "FAILED: no census line"; exit 1; }
grep -q "half the width only) is not built" "$out/two.log" || { echo "FAILED: value 2 does not say that it is not built"; exit 1; }
grep -q "far draws 18" "$out/census.log" || { echo "FAILED: the census did not count the far draws"; exit 1; }
# FXLOWRES_REF_HAS=1: the reference tree already has the switch (it is a tree at or after the lowres25 merge): the fixtures' stand-ins for its debug functions are not defined.
refdef=(-DFXLOWRES_BASE_TREE=1); [[ "${FXLOWRES_REF_HAS:-0}" == 1 ]] && refdef=()
# FXLOWRES_REF=<a checkout of the base commit>: the same fixture linked against THAT tree's back end (which has no such switch) must
# give the same trace and pixels as this tree with the switch unset: the default path is the base's, call for call.
if [[ -n "${FXLOWRES_REF:-}" ]]; then
  ref="$FXLOWRES_REF"
  nice -n 10 "${CXX:-g++}" -std=gnu++20 -O1 -g "${san[@]}" -DPORT_RUNTIME_DIAGNOSTICS=0 -DPORT_LOGGING=0 "${refdef[@]}" \
    -I"$ref/platform/vgx/src" -I"$ref/platform/vgx/third_party" -I"$ref/platform/include" -I"$khr" \
    "$root/tests/vgx_fxlowres_gl_test.cpp" "$ref/platform/vgx/src/vgx_backend_gl.cpp" "$ref/platform/vgx/src/vgx_autoq.cpp" "$ref/platform/vgx/src/vgx_shadergen.cpp" \
    "$ref/platform/vgx/host/vgx_host_egl.cpp" "${link[@]}" -lEGL -lGLESv2 -lpthread -o "$out/vgx_fxlowres_gl_ref" 2> "$out/ref-build.log"
  env -i PATH="$PATH" HOME="${HOME:-/tmp}" LIBGL_ALWAYS_SOFTWARE=1 LP_NUM_THREADS=1 ASAN_OPTIONS=detect_leaks=0 \
    GXV_LOG=0 GXV_FXHALF=0 GXV_WORKLOAD_PROBE=0 GXV_AUTOQ=0 GXV_KEEP_RT=0 "$out/vgx_fxlowres_gl_ref" "$out/ref" > "$out/ref.log" 2>&1 || { echo "FAILED: reference"; cat "$out/ref.log"; exit 1; }
  cmp "$out/ref.trace" "$out/unset.trace"
  cmp "$out/ref.rgba" "$out/unset.rgba"
  printf 'PASS: the base tree (%s) and this tree with the switch unset: byte-identical GL traces and RGBA readbacks\n' "$ref"
fi
# ---- GXV_FXLOWRES=1, the reduced-size path: pictures (tests/vgx_fxlowres_pic_gl_test.cpp, 960x544; tools/fxlowres_compare.py)
nice -n 10 "${CXX:-g++}" -std=gnu++20 -O1 -g "${san[@]}" -DPORT_RUNTIME_DIAGNOSTICS=0 -DPORT_LOGGING=0 \
  -I"$src" -I"$root/platform/vgx/third_party" -I"$root/platform/include" -I"$khr" \
  "$root/tests/vgx_fxlowres_pic_gl_test.cpp" "$src/vgx_backend_gl.cpp" "$src/vgx_autoq.cpp" "$src/vgx_shadergen.cpp" \
  "$root/platform/vgx/host/vgx_host_egl.cpp" "${link[@]}" -lEGL -lGLESv2 -lpthread -o "$out/vgx_fxlowres_pic_gl_test" 2> "$out/pic-build.log"
pic() { # <tag> env assignments...
  local tag=$1; shift
  env -i PATH="$PATH" HOME="${HOME:-/tmp}" LIBGL_ALWAYS_SOFTWARE=1 LP_NUM_THREADS=1 ASAN_OPTIONS=detect_leaks=0 \
    GXV_LOG=0 GXV_FXHALF=0 GXV_WORKLOAD_PROBE=0 GXV_AUTOQ=0 GXV_KEEP_RT=0 \
    "$@" "$out/vgx_fxlowres_pic_gl_test" "$out/pic-$tag" > "$out/pic-$tag.log" 2>&1 || { echo "FAILED: pic-$tag"; tail -5 "$out/pic-$tag.log"; exit 1; }
  grep -q -E "runtime error|AddressSanitizer" "$out/pic-$tag.log" && { echo "FAILED (sanitizer): pic-$tag"; exit 1; }
  grep "^PASS" "$out/pic-$tag.log"
}
pic off GXV_FXLOWRES=0
pic census GXV_FXLOWRES=3 GXV_FXLOWRES_EVERY=1
pic forced GXV_FXLOWRES=1 GXV_FXLOWRES_MIN=0 GXV_FXLOWRES_RUNS=16
pic gate GXV_FXLOWRES=1
pic nonear GXV_FXLOWRES=1 GXV_FXLOWRES_MIN=0 GXV_FXLOWRES_RUNS=16 GXV_FXLOWRES_NEAR=0
pic steponly GXV_FXLOWRES=1 GXV_FXLOWRES_MIN=0 GXV_FXLOWRES_STEP=1
cmp "$out/pic-off.rgba" "$out/pic-census.rgba"
cmp "$out/pic-off.rgba" "$out/pic-steponly.rgba" # (the controller's step is not on: nothing moves)
python3 "$root/tools/fxlowres_compare.py" "$out/pic-off.rgba" "$out/pic-forced.rgba" "$out/forced" | tee "$out/pic-forced.cmp"
python3 "$root/tools/fxlowres_compare.py" "$out/pic-off.rgba" "$out/pic-gate.rgba" "$out/gate" | tee "$out/pic-gate.cmp"
python3 "$root/tools/fxlowres_compare.py" "$out/pic-off.rgba" "$out/pic-nonear.rgba" | tee "$out/pic-nonear.cmp"
# forced: picture 1 changes a little (PSNR >= 30 dB); picture 2 (every effect behind a wall) and picture 3 (near effects only) are identical
grep -q "^picture 1: PSNR" "$out/pic-forced.cmp" || { echo "FAILED: forced: picture 1 did not change (nothing moved?)"; exit 1; }
awk '/^picture 1: PSNR/ { if ($4 + 0 < 30.0) { print "FAILED: forced: picture 1 PSNR " $4; exit 1 } }' "$out/pic-forced.cmp"
grep -q "^picture 2: identical" "$out/pic-forced.cmp" || { echo "FAILED: forced: an effect behind the wall shows"; exit 1; }
grep -q "^picture 3: identical" "$out/pic-forced.cmp" || { echo "FAILED: forced: a near effect was moved"; exit 1; }
grep -q " 0 runs at half size" "$out/pic-forced.log" && { echo "FAILED: forced: no run drawn at half size"; exit 1; }
# the default gate: at most one run a picture, at least 1.5 screens of fill
awk '/^picture 1: PSNR/ { if ($4 + 0 < 30.0) { print "FAILED: gate: picture 1 PSNR " $4; exit 1 } }' "$out/pic-gate.cmp"
if [[ -n "${FXLOWRES_REF:-}" ]]; then
  nice -n 10 "${CXX:-g++}" -std=gnu++20 -O1 -g "${san[@]}" -DPORT_RUNTIME_DIAGNOSTICS=0 -DPORT_LOGGING=0 "${refdef[@]}" \
    -I"$ref/platform/vgx/src" -I"$ref/platform/vgx/third_party" -I"$ref/platform/include" -I"$khr" \
    "$root/tests/vgx_fxlowres_pic_gl_test.cpp" "$ref/platform/vgx/src/vgx_backend_gl.cpp" "$ref/platform/vgx/src/vgx_autoq.cpp" "$ref/platform/vgx/src/vgx_shadergen.cpp" \
    "$ref/platform/vgx/host/vgx_host_egl.cpp" "${link[@]}" -lEGL -lGLESv2 -lpthread -o "$out/vgx_fxlowres_pic_gl_ref" 2> "$out/pic-ref-build.log"
  env -i PATH="$PATH" HOME="${HOME:-/tmp}" LIBGL_ALWAYS_SOFTWARE=1 LP_NUM_THREADS=1 ASAN_OPTIONS=detect_leaks=0 \
    GXV_LOG=0 GXV_FXHALF=0 GXV_WORKLOAD_PROBE=0 GXV_AUTOQ=0 GXV_KEEP_RT=0 "$out/vgx_fxlowres_pic_gl_ref" "$out/pic-ref" > "$out/pic-ref.log" 2>&1 || { echo "FAILED: picture reference"; exit 1; }
  cmp "$out/pic-ref.rgba" "$out/pic-off.rgba"
  printf 'PASS: the base tree and this tree with the switch off: byte-identical 960x544 pictures\n'
fi
# ---- freeze25: the stage-settable choice by shader key (GXV_FXLOWRES_K1..K4: low 24 bits of the fragment shader's content key, vgx_fxkeys.h)
pic keylog GXV_FXLOWRES=1 GXV_FXLOWRES_KEYLOG=1
keys=($(grep -o "candidate shader key [0-9]*" "$out/pic-keylog.log" | awk '{print $4}' | head -4))
[[ ${#keys[@]} -ge 1 ]] || { echo "FAILED: keylog found no candidate shader"; exit 1; }
kenv=(); i=1; for k in "${keys[@]}"; do kenv+=("GXV_FXLOWRES_K$i=$k"); i=$((i+1)); done
pic keyed GXV_FXLOWRES=1 GXV_FXLOWRES_RUNS=16 "${kenv[@]}"
pic keyother GXV_FXLOWRES=1 GXV_FXLOWRES_RUNS=16 GXV_FXLOWRES_K1=1 GXV_FXLOWRES_K2=16777215
pic keyoff GXV_FXLOWRES=0 GXV_FXLOWRES_RUNS=16 "${kenv[@]}"
# a key that matches nothing, or the switch off with keys set: the same picture as the gate alone / the switch off
cmp "$out/pic-gate.rgba" "$out/pic-keyother.rgba" || { echo "FAILED: an unlisted key changed the picture"; exit 1; }
cmp "$out/pic-off.rgba" "$out/pic-keyoff.rgba" || { echo "FAILED: keys with the switch off changed the picture"; exit 1; }
python3 "$root/tools/fxlowres_compare.py" "$out/pic-off.rgba" "$out/pic-keyed.rgba" "$out/keyed" | tee "$out/pic-keyed.cmp"
# listed shaders move whatever their distance and fill: picture 3 (near effects only, never moved by the near limit) now changes (a little), and
# picture 2 (all behind a wall) is still identical; no picture falls under 24 dB
grep -q "^picture 3: PSNR" "$out/pic-keyed.cmp" || { echo "FAILED: keyed: the near effects were not moved"; exit 1; }
grep -q "^picture 2: identical" "$out/pic-keyed.cmp" || { echo "FAILED: keyed: an effect behind the wall shows"; exit 1; }
awk '/^picture [0-9]: PSNR/ { if ($4 + 0 < 24.0) { print "FAILED: keyed: PSNR " $4; exit 1 } }' "$out/pic-keyed.cmp"
printf 'PASS: GXV_FXLOWRES=1: far effects at half size (forced and gated), hidden effects stay hidden, near effects stay at full size\n'
printf 'PASS: GXV_FXLOWRES unset / 0 / 3 / 2: byte-identical ordered GL traces and RGBA readbacks (the census changes nothing); counters as the fixture; GXV_FXLOWRES_CEILING leaves its class out\n'
