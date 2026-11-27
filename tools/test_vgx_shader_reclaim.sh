#!/usr/bin/env bash
# GXV_SHADER_RECLAIM (shader23): the real GLES back end on Mesa behind a model of vitaGL's shader / program slots (tests/vgx_shader_reclaim.cpp).
# A session of 12 stages x 3 loops with 2,840 distinct shaders (vitaGL has 2,048 slots):
#   mode 1 (default on): no glCreateShader = 0, slots bounded, every picture exact, revisits byte for byte the first visit;
#   mode tight: KEEP 256 / HIGH 512: the scene-change pass also deletes other stages' programs; same checks as mode 1;
#   mode emergency: the scene-change pass held off, the in-play last resort keeps every picture exact;
#   mode 0 (the control, as before): runs out of slots; every picture before the first lost draw must equal mode 1's.
#   tools/test_vgx_shader_reclaim.sh [output-dir]     (Khronos headers: KHR_INCLUDE, default /work/khr/include; CXX default g++)
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
src="$root/platform/vgx/src"
khr=${KHR_INCLUDE:-/work/khr/include}
units=(vgx_shadergen vgx_dlcache vgx_state vgx_draw vgx_fesplit vgx_guestmem vgx_bpsync vgx_texcache vgx_texdecode vgx_stats vgx_texcomp)
args=()
for unit in "${units[@]}"; do args+=("$src/$unit.cpp"); done
wraps=-Wl
for name in glDrawElements glFinish glGetError glReadPixels getenv glCreateProgram glDeleteProgram glCreateShader glDeleteShader glAttachShader \
            printf vprintf fprintf vfprintf puts putchar fputs fputc putc fwrite; do wraps+=",--wrap=$name"; done
if [[ "${RECLAIM_SKIP_BUILD:-0}" != 1 ]]; then
  nice -n 10 "${CXX:-g++}" -std=c++17 -O1 -g -w -DVGX_NO_NEON -D__VGX_VITA__ -DPORT_RUNTIME_DIAGNOSTICS=0 -DPORT_LOGGING=0 \
    -ffunction-sections -fdata-sections -Wl,--gc-sections "$wraps" \
    -I"$root/tests/autoq_clock_stubs" -I"$root/platform/include" -I"$src" -I"$root/platform/include" -I"$root/platform/vgx/third_party" -I"$khr" \
    "$root/tests/vgx_shader_reclaim.cpp" "${args[@]}" "$root/vita/vgx-game/silent_stdio.cpp" \
    -o "$out/vgx_shader_reclaim" -lEGL -lGLESv2 -pthread
fi
result=0
for mode in 1 tight emergency 0; do
  if env EGL_PLATFORM=surfaceless LIBGL_ALWAYS_SOFTWARE=1 LP_NUM_THREADS=1 timeout 900s "$out/vgx_shader_reclaim" "$mode" 12 3 "$out/visits-$mode.txt" \
       > "$out/reclaim-$mode.log" 2>&1; then
    echo "PASS mode $mode: $(grep '^{' "$out/reclaim-$mode.log")"
  else
    echo "FAIL mode $mode:"; cat "$out/reclaim-$mode.log"; result=1
  fi
done
# The control's pictures, up to its first lost draw, against mode 1's (the same visits).
if [[ -f "$out/visits-0.txt" && -f "$out/visits-1.txt" ]]; then
  first=$(awk '/^visit/ && $10 == 0 { print $2; exit }' "$out/visits-0.txt")
  first=${first:-999999}
  diffs=$(join <(awk -v f="$first" '/^visit/ && $2 < f { print $2, $8 }' "$out/visits-0.txt" | sort) \
               <(awk '/^visit/ { print $2, $8 }' "$out/visits-1.txt" | sort) | awk '$2 != $3' | wc -l)
  same=$(awk -v f="$first" '/^visit/ && $2 < f' "$out/visits-0.txt" | wc -l)
  if [[ "$diffs" == 0 && "$same" -gt 0 ]]; then echo "PASS control: the $same visits before its first lost draw (visit $first) have mode 1's pictures"
  else echo "FAIL control: $diffs of $same visits differ from mode 1"; result=1; fi
  for mode in 0 1 tight emergency; do
    [[ -f "$out/visits-$mode.txt" ]] && awk -v m="$mode" '/^visit/ { if ($16 > s) s = $16; if ($14 > h) h = $14 } END { print "mode " m ": most slots after a scene change " s ", most held " h }' "$out/visits-$mode.txt"
  done
fi
exit "$result"
