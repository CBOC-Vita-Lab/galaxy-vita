#!/usr/bin/env bash
# GXV_J3D_DIFFKEEP / GXV_J3D_MATMERGE / GXV_J3D_DIFFSKIP (the game-side cuts of the J3D state lists: decomp J3DPacket.cpp,
# Game/System/DrawBuffer.cpp, J3DPortDL.hpp) on the host: the draw-buffer-shaped scene of tests/vgx_j3d_lists.cpp through the real front end.
# Passes when every mode (keep, merge, skip, all) gives the base mode's command buffers frame for frame, for each seed and each setting of
# the front-end switches around them (GXV_SPLIT=0/1/2, GXV_SLDELTA=0, GXV_BPFAST=0, GXV_SLFAST=0, GXV_STATELIST=2: each replay checked against
# the parser); the cuts are made (counters); a deliberately wrong cut (JL_BROKEN=1) changes the command buffers; and an
# AddressSanitizer / UndefinedBehaviorSanitizer build reports nothing.
#   tools/test_vgx_j3d_lists.sh [frames] [seeds]      (CXX: host compiler, default g++; build in $OUT, default /tmp/vgx_j3d_lists)
# Instruction counts (callgrind, the player's thread):  JL_PROFILE=1 JL_MODE=base|all GXV_SPLIT=1 valgrind --tool=callgrind $OUT/jl 200 1
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
frames=${1:-120}
seeds=${2:-"1 2 3"}
out=${OUT:-/tmp/vgx_j3d_lists}
mkdir -p "$out"
cxx=${CXX:-g++}
srcs=(tests/vgx_j3d_lists.cpp platform/vgx/src/vgx_state.cpp platform/vgx/src/vgx_draw.cpp platform/vgx/src/vgx_fesplit.cpp
      platform/vgx/src/vgx_dlcache.cpp platform/vgx/src/vgx_guestmem.cpp platform/vgx/src/vgx_bpsync.cpp platform/vgx/src/vgx_shadergen.cpp
      platform/vgx/src/vgx_texcache.cpp platform/vgx/src/vgx_texdecode.cpp platform/vgx/src/vgx_stats.cpp platform/vgx/src/vgx_backend_null.cpp)
cd "$root"
common=(-std=c++17 -g -Iplatform/vgx/src -Iplatform/vgx/third_party -Idecomp/libs/JSystem/include -pthread)
$cxx "${common[@]}" -O2 "${srcs[@]}" -o "$out/jl"
$cxx "${common[@]}" -O1 -fsanitize=address,undefined -fno-omit-frame-pointer "${srcs[@]}" -o "$out/jl-asan"
export ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
fail=0
for seed in $seeds; do
  for around in "GXV_SPLIT=0" "GXV_SPLIT=1" "GXV_SPLIT=2" "GXV_SLDELTA=0" "GXV_BPFAST=0" "GXV_SLFAST=0" "GXV_STATELIST=2"; do
    env $around JL_MODE=base "$out/jl" "$frames" "$seed" > "$out/base.txt" 2> "$out/base.err"
    for mode in keep merge skip all; do
      env $around JL_MODE=$mode "$out/jl" "$frames" "$seed" > "$out/run.txt" 2> "$out/run.err"
      if ! cmp -s "$out/base.txt" "$out/run.txt"; then
        echo "FAIL: seed $seed, $around, JL_MODE=$mode: command buffers differ"; diff "$out/base.txt" "$out/run.txt" | head -4; fail=1
      fi
      if grep -q "MISMATCH" "$out/run.err"; then echo "FAIL: seed $seed, $around, JL_MODE=$mode: state-list replay mismatch"; fail=1; fi
    done
    grep -h "per picture" "$out/run.err" | sed "s/^/  seed $seed $around: /"
  done
  env JL_BROKEN=1 JL_MODE=all "$out/jl" "$frames" "$seed" > "$out/run.txt" 2> /dev/null
  if cmp -s "$out/base.txt" "$out/run.txt"; then echo "FAIL: seed $seed: the wrong cut (JL_BROKEN=1) was not seen"; fail=1; fi
  env JL_MODE=all "$out/jl-asan" 40 "$seed" > /dev/null 2> "$out/asan.err" || { echo "FAIL: seed $seed: sanitizer run"; tail -20 "$out/asan.err"; fail=1; }
done
[ $fail = 0 ] && echo "PASS: vgx_j3d_lists ($frames frames, seeds $seeds)"
exit $fail
