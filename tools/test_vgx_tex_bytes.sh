#!/usr/bin/env bash
# heap23b: the back end's live texture bytes (Stats::textureBytesLive, the heap line's "KiB of texels now") against uploads, re-uploads and
# frees, with the real GLES back end on Mesa (tests/vgx_tex_bytes.cpp). Same build as tools/test_vgx_shader_reclaim.sh.
#   tools/test_vgx_tex_bytes.sh [output-dir]     (Khronos headers: KHR_INCLUDE, default /work/khr/include; CXX default g++)
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
for name in glDrawElements glFinish glGetError glReadPixels getenv printf vprintf fprintf vfprintf puts putchar fputs fputc putc fwrite; do
  wraps+=",--wrap=$name"
done
nice -n 10 "${CXX:-g++}" -std=c++17 -O1 -g -w -DVGX_NO_NEON -D__VGX_VITA__ -DPORT_RUNTIME_DIAGNOSTICS=0 -DPORT_LOGGING=0 \
  -ffunction-sections -fdata-sections -Wl,--gc-sections "$wraps" ${SAN:+-fsanitize=$SAN} \
  -I"$root/tests/autoq_clock_stubs" -I"$root/platform/include" -I"$src" -I"$root/platform/include" -I"$root/platform/vgx/third_party" -I"$khr" \
  "$root/tests/vgx_tex_bytes.cpp" "${args[@]}" "$root/vita/vgx-game/silent_stdio.cpp" -o "$out/vgx_tex_bytes" -lEGL -lGLESv2 -pthread
EGL_PLATFORM=surfaceless LIBGL_ALWAYS_SOFTWARE=1 LP_NUM_THREADS=1 ASAN_OPTIONS=detect_leaks=0 timeout 600s "$out/vgx_tex_bytes"
