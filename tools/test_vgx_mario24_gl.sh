#!/usr/bin/env bash
# mario24 (tests/vgx_mario24_gl_test.cpp) on the real GLES back end and Mesa: the switches that act on the player's pixels only (GXV_PLAYER_LODBIAS,
# GXV_PLAYER_ANISO, GXV_EDGE_AA_PLAYER, GXV_PLAYER_SHARPEN) against a frame without them. No game data. Exit status 0 when every check passes.
#   tools/test_vgx_mario24_gl.sh [output-dir]
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
src="$root/platform/vgx/src"
bin="$out/vgx_mario24_gl_test"
nice -n 10 "${CXX:-g++}" -std=gnu++20 -O1 -g -w -DPORT_RUNTIME_DIAGNOSTICS=0 -DPORT_LOGGING=0 \
  -I"$src" -I"$root/platform/vgx/third_party" -I"$root/platform/include" \
  "$root/tests/vgx_mario24_gl_test.cpp" "$src/vgx_backend_gl.cpp" "$src/vgx_autoq.cpp" "$src/vgx_shadergen.cpp" \
  "$root/platform/vgx/host/vgx_host_egl.cpp" -Wl,--wrap=glTexParameteri -lEGL -lGLESv2 -lpthread -o "$bin"
status=0
run() { # run "<title>" "<ENV=..>..." args...
  local title=$1 envs=$2; shift 2
  echo "== $title"
  # shellcheck disable=SC2086
  env -i PATH="$PATH" HOME="${HOME:-/tmp}" LIBGL_ALWAYS_SOFTWARE=1 LP_NUM_THREADS=1 GXV_LOG=0 GXV_FXHALF_MIN=0 GXV_WORKLOAD_PROBE=0 GXV_KEEP_RT=0 \
    GXV_WIDE_RUNGS=1 $envs "$bin" "$@" 2>&1 | grep -E '^==|^  \[|all checks|FAILED|^\[mario24\]' || status=1
}
run "default environment: the player's texture gets no bias and no switch call" "" none
run "GXV_PLAYER_LODBIAS=-100 at 960x544 (rung 6): the player's texture -8/8, the other none" "GXV_PLAYER_LODBIAS=-100" lod 6 -8 0
run "GXV_PLAYER_LODBIAS=-50, with GXV_RUNG_LODBIAS=100 at 720x404 (rung 2: -3): player -7, the other -3" "GXV_PLAYER_LODBIAS=-50 GXV_RUNG_LODBIAS=100" lod 2 -7 -3
run "GXV_PLAYER_LODBIAS=+50 (softer): player +4 at 960x544" "GXV_PLAYER_LODBIAS=50" lod 6 4 0
run "total kept to -31 eighths" "GXV_PLAYER_LODBIAS=-300 GXV_RUNG_LODBIAS=200" lod 0 -31 -11
run "GXV_PLAYER_ANISO=3 (start-up filter off): the player's texture 3, then 0 again" "GXV_PLAYER_ANISO=3" aniso 3-0
run "GXV_PLAYER_ANISO=0 with GXV_ANISO=3 for the rest: the player's texture 0, then back to 3" "GXV_PLAYER_ANISO=0 GXV_ANISO=3" aniso 0-3
run "GXV_PLAYER_ANISO unset (GXV_ANISO=3): no call at all" "GXV_ANISO=3" aniso -
run "present, switches off, a player rectangle: nothing changes" "GXV_EDGE_AA=1 GXV_PRESENT_FAST=1 GXV_CAP_PRESENT=960x544" present off
run "GXV_EDGE_AA_PLAYER=1: the rectangle left out of the edge smoothing" "GXV_EDGE_AA=1 GXV_PRESENT_FAST=1 GXV_CAP_PRESENT=960x544 GXV_EDGE_AA_PLAYER=1" present leaveout
run "GXV_EDGE_AA_PLAYER=2: the lighter edge smoothing in the rectangle" "GXV_EDGE_AA=1 GXV_PRESENT_FAST=1 GXV_CAP_PRESENT=960x544 GXV_EDGE_AA_PLAYER=2" present light
run "GXV_PLAYER_SHARPEN=100 alone (acts as =1 with an unsharp mask)" "GXV_EDGE_AA=1 GXV_PRESENT_FAST=1 GXV_CAP_PRESENT=960x544 GXV_PLAYER_SHARPEN=100" present sharpen
exit $status
