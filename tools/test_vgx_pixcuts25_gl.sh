#!/usr/bin/env bash
# pixcuts25: GXV_CLEARQ (the EFB clear quad as a constant clear), GXV_TEVIN_PREWRAP (wrapped register copies) and GXV_INDCAP_SKIP / _REFRESH (the counter
# lines, the first-sight stale read) on the production front end and GLES back end on Mesa (tests/vgx_pixcuts25_gl.cpp, no game data). Picture hashes
# with each switch on must equal the switch-off pictures on every synthetic frame; negative controls must differ.
#   tools/test_vgx_pixcuts25_gl.sh [output-dir]     (Khronos headers: KHR_INCLUDE, default /work/khr/include; objects are kept in <output-dir>/obj)
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out/obj"
src="$root/platform/vgx/src"
khr=${KHR_INCLUDE:-/work/khr/include}
flags=(-std=c++20 -O1 -g0 -w -I"$src" -I"$root/platform/vgx/third_party" -I"$root/platform/include" -I"$khr")
sources=(vgx_state vgx_draw vgx_fesplit vgx_dlcache vgx_guestmem vgx_bpsync vgx_shadergen vgx_texcache vgx_texcomp vgx_texdecode vgx_stats vgx_backend_gl vgx_autoq)
objs=()
if [[ "${PIXCUTS_SKIP_BUILD:-0}" != 1 ]]; then
  for unit in "${sources[@]}"; do
    o="$out/obj/$unit.o"; objs+=("$o")
    # (rebuilt when the source or any header in the directory is newer)
    if [[ ! -f "$o" || -n "$(find "$src/$unit.cpp" "$src"/*.h "$src"/*.inc "$root/platform/include/port" -newer "$o" 2>/dev/null | head -1)" ]]; then
      nice -n 10 "${CXX:-g++}" "${flags[@]}" -c "$src/$unit.cpp" -o "$o" &
      while [[ $(jobs -r | wc -l) -ge ${JOBS:-2} ]]; do sleep 1; done
    fi
  done
  wait
  nice -n 10 "${CXX:-g++}" "${flags[@]}" -c "$root/tests/vgx_pixcuts25_gl.cpp" -o "$out/obj/test.o"
  [[ -f "$out/obj/host.o" ]] || nice -n 10 "${CXX:-g++}" "${flags[@]}" -c "$root/platform/vgx/host/vgx_host_egl.cpp" -o "$out/obj/host.o"
  "${CXX:-g++}" "$out"/obj/*.o -lEGL -lGLESv2 -pthread -o "$out/vgx_pixcuts25_gl"
fi
E="LIBGL_ALWAYS_SOFTWARE=1 LP_NUM_THREADS=1 GXV_LOG=0 GXV_SPLIT=0 GXV_AUTOQ=0 GXV_FXHALF=0 GXV_PASSHALF=0 GXV_CROWD=0 GXV_TEXMEMO=0 GXV_FIX_TEVIN=1 GXV_TEVIN_CANON=2"
run() { # <tag> <env...>
  local tag=$1; shift
  env $E "$@" "$out/vgx_pixcuts25_gl" 2>&1 | grep -v '^libunwind' > "$out/$tag.log" || true
  grep -E '^(frame|FAIL)' "$out/$tag.log" > "$out/$tag.hash" || true
}
fail=0
check() { [[ $1 == 1 ]] || { echo "FAILED: $2"; fail=1; }; }
run off  GXV_CLEARQ=0 GXV_TEVIN_PREWRAP=0
run cq   GXV_CLEARQ=1
run cq3  GXV_CLEARQ=3
run cq2  GXV_CLEARQ=2
run pw   GXV_TEVIN_PREWRAP=1
run both GXV_CLEARQ=1 GXV_TEVIN_PREWRAP=1
run cqs  GXV_CLEARQ=1 GXV_TEVIN_PREWRAP=1 GXV_SPLIT=1
run ic   GXV_INDCAP_SKIP=1 GXV_INDCAP_WARMUP=4
run icr  GXV_INDCAP_SKIP=1 GXV_INDCAP_WARMUP=4 GXV_INDCAP_REFRESH=2
for t in off cq cq2 cq3 pw both cqs ic icr; do grep -q FAIL "$out/$t.log" && { grep -H FAIL "$out/$t.log" | head -3; fail=1; }; done
n=$(grep -c '^frame' "$out/off.hash" || true); echo "frames with the switches off: $n"
check $([[ $n -ge 8 ]] && echo 1 || echo 0) "the test produced pictures"
for v in cq cq2 cq3 pw both cqs ic icr; do
  d=$(diff <(sed -E 's/ scenes=[0-9]+//' "$out/off.hash") <(sed -E 's/ scenes=[0-9]+//' "$out/$v.hash") | grep -c '^>' || true)
  echo "[$v] frames whose picture differs from the switch-off picture: $d"
  check $([[ $d -eq 0 ]] && echo 1 || echo 0) "$v changes a picture"
done
grep -h '^clearq' "$out/cq.log" "$out/cq2.log" || true
grep -h '^prewrap' "$out/pw.log" || true
# the controls: a wrong conversion must be caught
run brk GXV_CLEARQ=1 GXV_CLEARQ_BREAK=1
d=$(diff <(sed -E 's/ scenes=[0-9]+//' "$out/off.hash") <(sed -E 's/ scenes=[0-9]+//' "$out/brk.hash") | grep -c '^>' || true)
echo "[control CLEARQ_BREAK] frames that differ: $d"
check $([[ $d -gt 0 ]] && echo 1 || echo 0) "the negative control (a clear of the wrong colour) was not caught"
run pwb GXV_TEVIN_PREWRAP=1 GXV_PREWRAP_BREAK=1
d=$(diff <(sed -E 's/ scenes=[0-9]+//' "$out/off.hash") <(sed -E 's/ scenes=[0-9]+//' "$out/pwb.hash") | grep -c '^>' || true)
echo "[control PREWRAP_BREAK] frames that differ: $d"
check $([[ $d -gt 0 ]] && echo 1 || echo 0) "the negative control (an unwrapped copy) was not caught"
# INDCAP: with the default rules (predict window, stage learning) after a 4-picture warm-up every picture equals the switch-off picture while captures are
# left out; with the round-1 rules (no predict, no stage) the first-sight stale reads of the early reader show (a documented, inherent difference)
grep -h '^indcap' "$out/ic.log" || true
grep -Eq '^indcap: .* left=[1-9]' "$out/ic.log" || { echo "FAILED: GXV_INDCAP_SKIP=1 left no capture out"; fail=1; }
grep -Eq '^indcap-extra: refreshed=[1-9]' "$out/icr.log" || { echo "FAILED: GXV_INDCAP_REFRESH made no refresh capture"; fail=1; }
run icstrict GXV_INDCAP_SKIP=1 GXV_INDCAP_WARMUP=4 GXV_INDCAP_PREDICT=0 GXV_INDCAP_STAGE=0
d=$(diff <(sed -E 's/ scenes=[0-9]+//' "$out/off.hash") <(sed -E 's/ scenes=[0-9]+//' "$out/icstrict.hash") | grep -c '^>' || true)
echo "[round-1 rules, no predict / stage: the first-sight stale reads] frames that differ: $d (expected > 0: why PREDICT / STAGE / WARMUP are the defaults)"
check $([[ $d -gt 0 ]] && echo 1 || echo 0) "round-1 rules produced no stale read: the test frames do not cover it"
# scenes: with CLEARQ the clear quad is not drawn (the test counts draws through DebugLastFrameDraws when the backend has it)
for t in cq both cqs; do grep -q "converted>0" "$out/$t.log" || { echo "FAILED: $t converted no clear quad"; fail=1; }; done
grep -q "converted=0" "$out/cq2.log" || { echo "FAILED: count-only mode converted a quad"; fail=1; }
grep -q "dual rows>0" "$out/pw.log" || { echo "FAILED: PREWRAP made no dual key"; fail=1; }
[[ $fail == 0 ]] && echo "ok: pixcuts25"
exit $fail
