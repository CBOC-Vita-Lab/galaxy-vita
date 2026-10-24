#!/usr/bin/env bash
# The vitaGL of the 1.0 release (variant name "nhustd"):
#   upstream vitaGL at 76f43e6 (https://github.com/Rinnegatamante/vitaGL)
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
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
name=${1:-nhustd}
upstream=${VGL_UPSTREAM:?set VGL_UPSTREAM to a git clone of https://github.com/Rinnegatamante/vitaGL}
base=76f43e6fbe1513cee44118da7ff2c16728c4fed2
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
git -C "$upstream" archive --format=tar "$base" | tar -x -C "$work"
for p in "$here"/patches/*.patch; do
  (cd "$work" && git apply "$p") || { echo "patch did not apply: $p"; exit 1; }
done
python3 -I "$here/gpu_timers.py" "$work"
python3 -I "$here/fbo_msaa.py" "$work"
VGL_SRC="$work/source" "$here/build-vitagl.sh" "$name" \
  "DEPTH_HINT UNIFORM_PINGPONG ALIGNED_POOL GPU_TIMERS ANISO_SWITCH FBO_MSAA DEPTH_SAMPLE MASK_SKIP"
