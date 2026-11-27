#!/usr/bin/env bash
# sharp24 (tests/vgx_sharp24_gl_test.cpp) on the real GLES back end and Mesa: the texture LOD bias per rung and the size gate of anisotropic
# filtering. Each scenario runs the test binary with the environment it needs. No game data. Exit status 0 when every check passes.
#   tools/test_vgx_sharp24_gl.sh [output-dir]
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
src="$root/platform/vgx/src"
bin="$out/vgx_sharp24_gl_test"
nice -n 10 "${CXX:-g++}" -std=gnu++20 -O1 -g -w -DPORT_RUNTIME_DIAGNOSTICS=0 -DPORT_LOGGING=0 \
  -I"$src" -I"$root/platform/vgx/third_party" \
  "$root/tests/vgx_sharp24_gl_test.cpp" "$src/vgx_backend_gl.cpp" "$src/vgx_autoq.cpp" "$src/vgx_shadergen.cpp" \
  "$root/platform/vgx/host/vgx_host_egl.cpp" -Wl,--wrap=glTexParameteri -lEGL -lGLESv2 -lpthread -o "$bin"
status=0
run() { # run "<title>" "<ENV=..>..." args...
  local title=$1 envs=$2; shift 2
  echo "== $title"
  # shellcheck disable=SC2086
  env -i PATH="$PATH" HOME="${HOME:-/tmp}" LIBGL_ALWAYS_SOFTWARE=1 LP_NUM_THREADS=1 GXV_LOG=0 GXV_FXHALF_MIN=0 GXV_WORKLOAD_PROBE=0 GXV_KEEP_RT=0 \
    GXV_WIDE_RUNGS=1 $envs "$bin" "$@" 2>&1 | grep -E '^==|^  \[|all checks|FAILED|^\[sharp24\]' || status=1
}
run "default environment: no LOD bias at any rung" "" none
run "GXV_RUNG_LODBIAS=60 (the console default): 720x404 -2, 608x348 -3 (0.395 level), 960x544 0" "GXV_RUNG_LODBIAS=60" lod 2 -2
run "  608x348" "GXV_RUNG_LODBIAS=60" lod 0 -3
run "  960x544" "GXV_RUNG_LODBIAS=60" lod 6 0
run "GXV_RUNG_LODBIAS=100: 720x404 -3, 608x348 -5" "GXV_RUNG_LODBIAS=100" lod 2 -3
run "  608x348" "GXV_RUNG_LODBIAS=100" lod 0 -5
run "GXV_RUNG_LODBIAS=100 GXV_RUNG_LODBIAS_EXTRA=2: 720x404 -5, 960x544 -2 (the top rung gets it too)" "GXV_RUNG_LODBIAS=100 GXV_RUNG_LODBIAS_EXTRA=2" lod 2 -5
run "  960x544" "GXV_RUNG_LODBIAS=100 GXV_RUNG_LODBIAS_EXTRA=2" lod 6 -2
run "EXTRA alone (GXV_RUNG_LODBIAS=0): 960x544 -4" "GXV_RUNG_LODBIAS_EXTRA=4" lod 6 -4
run "total kept to -31 eighths (the old range wrap at -32)" "GXV_RUNG_LODBIAS=200 GXV_RUNG_LODBIAS_EXTRA=24" lod 0 -31
run "EXTRA=4 EXTRA_AF=2 at 960x544: off -4, on -2, off -4" "GXV_RUNG_LODBIAS_EXTRA=4 GXV_RUNG_LODBIAS_EXTRA_AF=2" lodaf -4 -2
run "EXTRA=3, no EXTRA_AF: the same with it on" "GXV_RUNG_LODBIAS_EXTRA=3" lodaf -3 -3
run "GXV_ANISO_MINSIZE=0: no gate" "GXV_ANISO_MINSIZE=0" gate 0
run "GXV_ANISO_MINSIZE=128 (census on): the 64-texel texture is set plain" "GXV_ANISO_MINSIZE=128 GXV_ANISO_CENSUS=1" gate 128
exit $status
