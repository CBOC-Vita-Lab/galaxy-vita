#!/usr/bin/env bash
# The vitaGL of the 1.0 release (variant name "nhustd"):
#   upstream vitaGL at 9bf3e3f (30 Oct 2026, glUseProgram dirty-flag skip; 1.0 and 1.1 used 76f43e6) (https://github.com/Rinnegatamante/vitaGL)
#   + patches/*.patch        the Strikers port's fixes: uniform pool lifetime, blit flip, shader cache file names,
#                            allocation recovery, title id buffer, shader cache and shader/program robustness,
#                            shader source length
#   + gpu_timers.py          GPU_TIMERS (per-scene GPU times for the automatic quality) and ANISO_SWITCH (off unless asked for)
#   + fbo_msaa.py            FBO_MSAA (multisampling per framebuffer object, for the automatic anti-aliasing)
#   built by build-vitagl.sh with DEPTH_HINT UNIFORM_PINGPONG ALIGNED_POOL GPU_TIMERS ANISO_SWITCH FBO_MSAA DEPTH_SAMPLE MASK_SKIP
#   (the last five and ALIGNED_POOL / DEPTH_HINT / UNIFORM_PINGPONG are patched in by build-vitagl.sh itself).
#
#   VGL_UPSTREAM=<a vitaGL git clone> VGL_LIBS=<lib/ + include/ of the other libraries> tools/vitagl/build-release.sh [name]
#       -> build/vitagl/<name>/ (default name nhustd), a complete VGL_DEPS folder for tools/vgx_build/build.py.
# The clone is only read (git archive); nothing is downloaded.
#
# Variant "nhustdx" (the 1.2 release and the default name; FAST=0 or the name nhustd gives the older set): the same library with three upstream
# speed options on, DRAW_SPEEDHACK INDICES_SPEEDHACK SAMPLERS_SPEEDHACK, and GXM_STATE_CACHE (patches/0011: no repeated
# sceGxmSetFragmentProgram / SetVertexProgram / SetFragmentTexture). Each was checked against how the Galaxy back end
# calls vitaGL and gives the same draws for it (tools/vitagl/README.md has the reasons). HAVE_SCRATCH_MEMORY is compiled in too but
# does nothing until the back end asks for it (GXV_VITA_SCRATCH=1 makes the frame's stream buffers come from the per-picture pool).
# CIRCULAR_POOL_SPEEDHACK is not in the set (README: it can overwrite a texture upload the GPU has not read yet). STRICT_DRAW_COMPLIANCE stays on:
# without it the Vita build's relative vertex offsets overflow vitaGL's 16-bit attribute offset. Not part of nhustd.
# VGL_EXTRA="OPT1 OPT2" adds more options to either variant (for A/B builds; the README lists which are not safe).
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
fast=${FAST:-1}  # 1.2 ships nhustdx; FAST=0 (or the name nhustd) builds the 1.0/1.1 option set
if [ -n "${1:-}" ]; then name=$1; elif [ "$fast" = 1 ]; then name=nhustdx; else name=nhustd; fi
[ "$name" = nhustdx ] && fast=1
options="DEPTH_HINT UNIFORM_PINGPONG ALIGNED_POOL GPU_TIMERS ANISO_SWITCH FBO_MSAA DEPTH_SAMPLE MASK_SKIP"
[ "$fast" = 1 ] && options="$options DRAW_SPEEDHACK INDICES_SPEEDHACK SAMPLERS_SPEEDHACK GXM_STATE_CACHE HAVE_SCRATCH_MEMORY"
options="$options ${VGL_EXTRA:-}"
upstream=${VGL_UPSTREAM:?set VGL_UPSTREAM to a git clone of https://github.com/Rinnegatamante/vitaGL}
base=9bf3e3f4e55efd1f4a191b179144f7f9a98cd925
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
git -C "$upstream" archive --format=tar "$base" | tar -x -C "$work"
for p in "$here"/patches/*.patch; do
  (cd "$work" && git apply "$p") || { echo "patch did not apply: $p"; exit 1; }
done
python3 -I "$here/gpu_timers.py" "$work"
python3 -I "$here/fbo_msaa.py" "$work"
VGL_SRC="$work/source" "$here/build-vitagl.sh" "$name" "$options"
