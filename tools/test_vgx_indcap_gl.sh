#!/usr/bin/env bash
# GXV_INDCAP_SKIP / GXV_INDCAP_HALFRATE (gpu23): the marked "Indirect" capture through the production front end and GLES back end on
# Mesa (tests/vgx_indcap_gl.cpp). Pictures of frames 1, 2, 3, 5 and 7 must be identical with the switches off and on; with
# GXV_INDCAP_SKIP=1 frame 1 (a capture nothing reads) must have fewer scenes; =2 must change no scene count.
#   tools/test_vgx_indcap_gl.sh [output-dir]     (Khronos headers: KHR_INCLUDE, default /work/khr/include)
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
src="$root/platform/vgx/src"
khr=${KHR_INCLUDE:-/work/khr/include}
sources=(vgx_state vgx_draw vgx_fesplit vgx_dlcache vgx_guestmem vgx_bpsync vgx_shadergen vgx_texcache vgx_texcomp vgx_texdecode vgx_stats vgx_backend_gl vgx_autoq)
args=()
for unit in "${sources[@]}"; do args+=("$src/$unit.cpp"); done
if [[ "${INDCAP_SKIP_BUILD:-0}" != 1 ]]; then
  nice -n 10 "${CXX:-g++}" -std=c++20 -O1 -I"$src" -I"$root/platform/include" -I"$root/platform/vgx/third_party" -I"$khr" \
    "$root/tests/vgx_indcap_gl.cpp" "${args[@]}" "$root/platform/vgx/host/vgx_host_egl.cpp" \
    -lEGL -lGLESv2 -pthread -o "$out/vgx_indcap_gl"
fi
# (GXV_INDCAP_WARMUP=0 in the runs that look at the rules after a stage's first 10 s; "w" keeps the default and must leave nothing
# out in these 8 pictures, no stale read, every picture identical)
W=GXV_INDCAP_WARMUP=0
runs=("0:GXV_INDCAP_SKIP=0" "1:GXV_INDCAP_SKIP=1 $W" "2:GXV_INDCAP_SKIP=2 $W" "old:GXV_INDCAP_SKIP=1 GXV_INDCAP_PREDICT=0 GXV_INDCAP_STAGE=0 $W"
      "st:GXV_INDCAP_SKIP=1 GXV_INDCAP_PREDICT=0 $W" "pr:GXV_INDCAP_SKIP=1 GXV_INDCAP_STAGE=0 $W" "w:GXV_INDCAP_SKIP=1"
      "h:GXV_INDCAP_HALFRATE=1" "1h:GXV_INDCAP_SKIP=1 GXV_INDCAP_HALFRATE=1 $W"
      "hg:GXV_INDCAP_HALFRATE=1 GXV_INDCAP_CAMGATE=1" "hgc:GXV_INDCAP_HALFRATE=1 GXV_INDCAP_CAMGATE=1 TEST_CUT7=1")
for r in "${runs[@]}"; do
  v=${r%%:*}
  env LIBGL_ALWAYS_SOFTWARE=1 LP_NUM_THREADS=1 GXV_LOG=0 GXV_SPLIT=0 GXV_AUTOQ=0 GXV_FXHALF=0 GXV_PASSHALF=0 GXV_CROWD=0 \
    INDCAP_DUMP="$out/dump-$v" ${r#*:} "$out/vgx_indcap_gl" > "$out/indcap-$v.log" 2>&1 || true; grep -v "^libunwind" "$out/indcap-$v.log" | grep -v "PASS: picture read back" | sed "s/^/[$v] /"
done
grep -q FAIL "$out"/indcap-*.log && { echo "FAILED: a check"; exit 1; }
# Frames 2-4: frame 2 is the first picture of a reader of the previous capture after a left-out one (one stale read under any
# rule); what it drew is captured and read again by frames 3 and 4 (feedback, a quarter the size each picture). Pixels that differ
# from the run without the switch, per frame: the round-1 rule (old) adds stale reads in frames 3 and 4 as well.
diffs() { python3 -I - "$out/dump-0" "$out/dump-$1" <<'PY'
import sys
a, b = sys.argv[1], sys.argv[2]
r = []
for f in (2, 3, 4):
    x = open(f"{a}-f{f}.rgba", "rb").read(); y = open(f"{b}-f{f}.rgba", "rb").read()
    r.append(sum(1 for i in range(0, len(x), 4) if x[i:i+3] != y[i:i+3]))
print(" ".join(map(str, r)))
PY
}
for v in 1 2 old st pr; do echo "pixels differing from run 0 in frames 2 3 4 ($v): $(diffs $v)"; done
[[ "$(diffs 2)" == "0 0 0" ]] || { echo "FAILED: GXV_INDCAP_SKIP=2 changed a picture"; exit 1; }
[[ "$(diffs w)" == "0 0 0" ]] || { echo "FAILED: the default warm-up should leave nothing out in a stage's first pictures"; exit 1; }
grep -q "left=0 .*stale=0" "$out/indcap-w.log" || { echo "FAILED: the default warm-up left a capture out or read stale"; exit 1; }
# (frame 2 is the one picture a new reader of a left-out capture can see an older one; frames 5 and 7 are the half rate's)
pics() { grep -E '^frame[18] ' "$out/indcap-$1.log" | sed -E "s/scenes=[0-9]+ //" | sort; }
pics57() { grep -E '^frame[57] ' "$out/indcap-$1.log" | sed -E "s/scenes=[0-9]+ //" | sort; }
f3() { grep -E '^frame3 ' "$out/indcap-$1.log" | sed -E 's/.*hash=//'; }
scenes() { grep -E "^frame$2 " "$out/indcap-$1.log" | sed -E 's/.*scenes=([0-9]+).*/\1/'; }
h0=$(pics 0)
for v in 1 2 st pr w h 1h; do [[ "$h0" == "$(pics $v)" ]] || { echo "FAILED: pictures differ ($v)"; echo "$h0"; pics $v; exit 1; }; done
for v in 1 2 st pr; do [[ "$(pics57 0)" == "$(pics57 $v)" ]] || { echo "FAILED: frames 5/7 differ ($v)"; exit 1; }; done
[[ "$(f3 0)" != "$(f3 old)" ]] || { echo "FAILED: the round-1 rule should reproduce the wrong frame 3"; exit 1; }
for v in 1 st pr; do
  grep -q "frame3 stale=0" "$out/indcap-$v.log" || { echo "FAILED: a second stale read with the round-2 rule ($v)"; exit 1; }
done
grep -q "frame3 stale=1" "$out/indcap-old.log" || { echo "FAILED: the round-1 rule should reproduce the second stale read"; exit 1; }
s0=$(scenes 0 1); s1=$(scenes 1 1); s2=$(scenes 2 1)
[[ $s1 -lt $s0 ]] || { echo "FAILED: frame 1 scenes $s0 -> $s1 with GXV_INDCAP_SKIP=1 (expected fewer)"; exit 1; }
[[ $s2 -eq $s0 ]] || { echo "FAILED: frame 1 scenes $s0 -> $s2 with GXV_INDCAP_SKIP=2 (expected the same)"; exit 1; }
echo "PASS: identical pictures (0 1 2 st pr w h 1h; frames 1 8, and 5 7 without the half rate; =2 all frames), one stale read (first sight) vs the round-1 rule's three, the round-1 rule reproduces the stale frame 3, frame 1 scenes $s0 -> $s1 with GXV_INDCAP_SKIP=1"
