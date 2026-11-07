#!/usr/bin/env bash
# GXV_TEXBAKE (the texture matrix of a CPU-transformed draw is applied to its texture coordinates, and its block holds identity rows,
# so that particles which differ only in their texture matrix merge): the production GX front end and the production GLES back end on
# Mesa (tests/vgx_texbake_gl.cpp). Emitters of particles as the game draws them; the picture must be bit-identical with the switch off
# and on (TB_MODE=exact: matrices whose products are exact), under the other front-end switches (GXV_SPLIT 0/1/2, GXV_RSCACHE 0/1/2,
# GXV_SOFTXFORM 128/32/0, GXV_FOLDREGS 0, GXV_FOLDAHEAD 0, GXV_RESOLVEFAST 2, GXV_RIGIDMERGE 1/2, three more scenes), and the number of GL draws must fall with it on (and be
# the same with it off and with nothing CPU-transformed). TB_MODE=random (arbitrary matrices) is compared the same way and reported.
#   tools/test_vgx_texbake_gl.sh [output-dir]       (no game data, no Vita; Khronos headers: KHR_INCLUDE, default /work/khr/include)
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
src="$root/platform/vgx/src"
khr=${KHR_INCLUDE:-/work/khr/include}
sources=(vgx_state vgx_draw vgx_fesplit vgx_dlcache vgx_guestmem vgx_bpsync vgx_shadergen vgx_texcache vgx_texdecode vgx_stats vgx_backend_gl vgx_autoq)
args=()
for unit in "${sources[@]}"; do args+=("$src/$unit.cpp"); done
if [[ "${GXV_TEXBAKE_SKIP_BUILD:-0}" != 1 ]]; then
  nice -n 10 "${CXX:-g++}" -std=c++20 -O1 -g -I"$src" -I"$root/platform/vgx/third_party" -I"$khr" \
    "$root/tests/vgx_texbake_gl.cpp" "${args[@]}" "$root/platform/vgx/host/vgx_host_egl.cpp" \
    -lEGL -lGLESv2 -pthread -o "$out/vgx_texbake_gl"
fi
run() { # <tag> env assignments...
  local tag=$1; shift
  env -u GXV_TEXBAKE -u GXV_RIGIDMERGE -u GXV_SPLIT -u GXV_RSCACHE -u GXV_SOFTXFORM -u GXV_FOLDREGS -u GXV_FOLDAHEAD -u GXV_RESOLVEFAST \
    LIBGL_ALWAYS_SOFTWARE=1 LP_NUM_THREADS=1 GXV_LOG=0 GXV_AUTOQ=0 GXV_FXHALF=0 GXV_PASSHALF=0 GXV_CROWD=0 GXV_EFBSWAP=0 \
    "$@" TB_DUMP="$out/$tag.raw" "$out/vgx_texbake_gl" 2>&1 | grep -v '^libunwind' > "$out/$tag.log" || true
  grep -q FAIL "$out/$tag.log" && { grep FAIL "$out/$tag.log"; echo "FAILED: $tag"; exit 1; }
  grep -q '^picture' "$out/$tag.log" || { echo "FAILED: $tag produced no picture"; tail -5 "$out/$tag.log"; exit 1; }
  grep -E '^picture' "$out/$tag.log" | sed 's/.*hash //' > "$out/$tag.hash"
  grep -E '^records' "$out/$tag.log" | sed 's/records //' > "$out/$tag.records"
}
for mode in exact random; do
  for soft in 128 32 0; do
    for split in 0 1 2; do
      for rs in 0 1 2; do
        for tb in 0 1; do run "$mode-soft$soft-split$split-rs$rs-tb$tb" TB_MODE=$mode GXV_TEXBAKE=$tb GXV_SOFTXFORM=$soft GXV_SPLIT=$split GXV_RSCACHE=$rs; done
      done
    done
  done
  for seed in 7 99 31337; do for tb in 0 1; do run "$mode-seed$seed-soft32-tb$tb" TB_MODE=$mode TB_SEED=$seed GXV_TEXBAKE=$tb GXV_SOFTXFORM=32; done; done
  for tb in 0 1; do
    run "$mode-nofold-tb$tb" TB_MODE=$mode GXV_TEXBAKE=$tb GXV_FOLDREGS=0
    run "$mode-noahead-tb$tb" TB_MODE=$mode GXV_TEXBAKE=$tb GXV_FOLDAHEAD=0
    run "$mode-rfast2-tb$tb" TB_MODE=$mode GXV_TEXBAKE=$tb GXV_RESOLVEFAST=2 GXV_RSCACHE=0
    run "$mode-rigid1-tb$tb" TB_MODE=$mode GXV_TEXBAKE=$tb GXV_RIGIDMERGE=1 GXV_SOFTXFORM=32
    run "$mode-rigid2-tb$tb" TB_MODE=$mode GXV_TEXBAKE=$tb GXV_RIGIDMERGE=2 GXV_SPLIT=1
  done
done
# pictures: exact mode, every run the same picture
ref=$(cat "$out/exact-soft128-split0-rs1-tb0.hash")
n=0
for f in $(ls "$out"/exact-*.hash | grep -v seed); do
  n=$((n + 1))
  [[ "$(cat "$f")" == "$ref" ]] || { echo "FAIL: $(basename "$f" .hash) picture $(cat "$f") differs from $ref"; exit 1; }
done
echo "exact matrices: pictures identical across $n runs: $ref"
rref=$(cat "$out/random-soft128-split0-rs1-tb0.hash")
same=0; diff=0
for f in $(ls "$out"/random-*.hash | grep -v seed); do
  if [[ "$(cat "$f")" == "$rref" ]]; then same=$((same + 1)); else diff=$((diff + 1)); echo "  random matrices: $(basename "$f" .hash) differs"; fi
done
echo "random matrices: $same runs identical to the switch-off picture, $diff differ"
if [[ $diff -ne 0 ]]; then
  python3 - "$out/random-soft128-split0-rs1-tb0.raw" "$out/random-soft128-split0-rs1-tb1.raw" <<'EOF'
import sys
a = open(sys.argv[1], 'rb').read(); b = open(sys.argv[2], 'rb').read()
d = [abs(x - y) for x, y in zip(a, b)]
px = sum(1 for i in range(0, len(d), 4) if max(d[i:i + 4]) > 0)
print(f"  random matrices: {px} of {len(a) // 4} pixels differ, largest channel difference {max(d)}")
EOF
fi
for mode in exact random; do for seed in 7 99 31337; do
  a=$(cat "$out/$mode-seed$seed-soft32-tb0.hash"); b=$(cat "$out/$mode-seed$seed-soft32-tb1.hash")
  if [[ "$a" == "$b" ]]; then echo "$mode matrices, scene $seed: identical ($a)"
  elif [[ $mode == exact ]]; then echo "FAIL: exact matrices, scene $seed: $a without the switch, $b with it"; exit 1
  else echo "  random matrices, scene $seed: pictures differ ($a / $b)"; fi
done; done
# GL draws: the switch-off count for every variant without it; fewer with it where draws are CPU-transformed; the same with GXV_SOFTXFORM=0
for mode in exact random; do
  for soft in 128 32 0; do
    off=$(cat "$out/$mode-soft$soft-split0-rs1-tb0.records"); on=$(cat "$out/$mode-soft$soft-split0-rs1-tb1.records")
    for split in 0 1 2; do for rs in 0 1 2; do
      [[ "$(cat "$out/$mode-soft$soft-split$split-rs$rs-tb0.records")" == "$off" ]] || { echo "FAIL: $mode soft$soft split$split rs$rs: draws differ from $off"; exit 1; }
      [[ "$(cat "$out/$mode-soft$soft-split$split-rs$rs-tb1.records")" == "$on" ]] || { echo "FAIL: $mode soft$soft split$split rs$rs (on): draws differ from $on"; exit 1; }
    done; done
    if [[ $soft == 0 ]]; then [[ $on == "$off" ]] || { echo "FAIL: $mode GXV_SOFTXFORM=0: $on draws with the switch, $off without"; exit 1; }
    else [[ $on -lt $off ]] || { echo "FAIL: $mode soft$soft: $on draws, not fewer than $off"; exit 1; }; fi
    echo "GL draws ($mode, GXV_SOFTXFORM=$soft): GXV_TEXBAKE=0 $off, GXV_TEXBAKE=1 $on"
  done
done
echo "PASS"
