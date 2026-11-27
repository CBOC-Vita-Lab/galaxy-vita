#!/usr/bin/env bash
# GXV_RT_DEFER (tests/vgx_rt_defer.cpp): the back end as for the console on the GL stub, against a model of vitaGL's texture storage
# (an unsampled texture's storage freed at once) and of a GPU a frame behind. GXV_RT_DEFER=0 (the 2.2 behaviour) must show GPU writes
# into freed storage - the Comet Observatory crash's mechanism -, the default (4) none. With ASan (OBS_ASAN=1, the default when the
# compiler has it) the 2.2 run also makes the writes for real (from frame 2: past the start-up target) and must die with a
# heap-use-after-free report whose free is the rung change's retirement of a texture rewritten every picture.
#   tools/test_vgx_rt_defer.sh [output-dir]
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
flags=(-std=c++17 -O1 -g -fpermissive -w -DNDEBUG -D__VGX_VITA__=1 -I"$root/armhf/glstub" -I"$root/tests/autoq_clock_stubs" -I"$root/platform/include"
       -I"$root/platform/vgx/src" -I"$root/platform/vgx/third_party")
srcs=("$root/tests/vgx_rt_defer.cpp" "$root/platform/vgx/src/vgx_shadergen.cpp" "$root/platform/vgx/src/vgx_autoq.cpp")
# (fix32) clang++ builds the backend here (g++ rejects its `using namespace vgx::backend` + weak extern "C" port_scene_serial); the default
# compiler is clang++ when there is one. ASan is used only when that compiler can link it (a host without the sanitizer runtime runs the
# model's own hazard count, which is what the first two runs check).
cxx=${CXX:-}
if [ -z "$cxx" ]; then if command -v clang++ >/dev/null; then cxx=clang++; else cxx=g++; fi; fi
asan=${OBS_ASAN:-1}
if [ "$asan" = 1 ]; then
  probe=$(mktemp -d); echo 'int main(){return 0;}' > "$probe/p.cpp"
  if ! "$cxx" -fsanitize=address "$probe/p.cpp" -o "$probe/p" >/dev/null 2>&1; then echo "note: $cxx cannot link ASan here: running without it"; asan=0; fi
  rm -rf "$probe"
fi
if [ "$asan" = 1 ]; then
  nice -n 10 "$cxx" "${flags[@]}" -fsanitize=address -fno-omit-frame-pointer "${srcs[@]}" -o "$out/vgx_rt_defer" -lpthread
else
  nice -n 10 "$cxx" "${flags[@]}" "${srcs[@]}" -o "$out/vgx_rt_defer" -lpthread
fi
status=0
export ASAN_OPTIONS=${ASAN_OPTIONS:-detect_leaks=0}
echo "== GXV_RT_DEFER=4 (the fix)"
"$out/vgx_rt_defer" 4 || status=1
echo "== GXV_RT_DEFER=0 (2.2): hazards expected"
"$out/vgx_rt_defer" 0 || status=1
if [ "$asan" = 1 ]; then
  echo "== GXV_RT_DEFER=0 with the GPU writes made (ASan must report a heap-use-after-free)"
  if OBS_GPU_WRITE=2 "$out/vgx_rt_defer" 0 >"$out/asan0.txt" 2>&1; then echo "FAIL: no ASan report"; status=1
  else grep -m1 -A30 "heap-use-after-free" "$out/asan0.txt" | grep -E "ERROR|WRITE|located|freed by|#[0-9] .*(RetireTexture|DeleteGpuTex|DeleteTex|RungSkipResample|ApplyRung|CopyTexture|M_glDeleteTextures|M_glTexImage2D)" || { echo "FAIL: no use-after-free report"; tail -20 "$out/asan0.txt"; status=1; }; fi
  echo "== GXV_RT_DEFER=4 with the GPU writes made (ASan must stay quiet)"
  OBS_GPU_WRITE=1 "$out/vgx_rt_defer" 4 >"$out/asan4.txt" 2>&1 || { echo "FAIL"; tail -30 "$out/asan4.txt"; status=1; }
  grep -q "heap-use-after-free" "$out/asan4.txt" && { echo "FAIL: ASan report with the fix"; status=1; }
  tail -1 "$out/asan4.txt"
fi
exit $status
