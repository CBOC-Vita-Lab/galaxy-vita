#!/usr/bin/env bash
# Whole recording of the README ("Record a profile again"), for a machine that has the armhf toolchain (clang with the arm-linux-gnueabihf
# target, arm-linux-gnueabihf-g++ and its sysroot), qemu-arm (or qemu-arm-static), VITASDK and the cooked game data.  The chain was run end
# to end on 18 Nov 2026 (pgotool31: the instrumented build, the opening route, the merge); the full route list has not been played yet.
#   tools/pgo/record_all.sh <work-dir> <cooked-data-dir>
# Environment: ROUTES="name:seconds ..." (default: every route below), PICTURE_MODE=60|30 (the console's picture mode the routes are
# played in, default 60 = Performance, the release default; tools/pgo/gen_vita_env.py), JOBS (ninja, default 2), QEMU_ARM, SYSROOT.
# <work-dir> is reused: the build is incremental, a route whose run directory exists is skipped (delete it to play it again).
set -euo pipefail
[ $# -eq 2 ] || { sed -n '2,8p' "$0"; exit 2; }
W=$1; DATA=$2; SRC=$(cd "$(dirname "$0")/../.." && pwd); mkdir -p "$W"; W=$(cd "$W" && pwd)
stamp() { echo "== $(date '+%F %T') $*"; }
stamp "profile runtime"
[ -f "$W/rt/libclang_rt.profile-armhf.a" ] || "$SRC/tools/pgo/build_profile_rt.sh" "$W/rt"
[ -d "$W/case-shim" ] || python3 -I "$SRC/tools/vgx_build/make_case_shim.py" "$SRC" "$W/case-shim" > "$W/case-shim.log"
export CPATH="$W/case-shim" VITASDK=${VITASDK:-/opt/vitasdk}
stamp "configure"
cmake -S "$SRC/armhf" -B "$W/obj" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$SRC/armhf/toolchain.cmake" -DCMAKE_BUILD_TYPE= \
      -DARMHF_PGO_GEN=ON -DARMHF_PGO_GEN_RENDERER=ON -DARMHF_VGX_BACKEND=gl-stub -DARMHF_VITA_RUNTIME_FLAGS=ON \
      -DARMHF_PROFILE_RT="$W/rt/libclang_rt.profile-armhf.a" > "$W/configure.log"
stamp "instrumented build (log: $W/build.log)"
nice ninja -C "$W/obj" -j"${JOBS:-2}" game > "$W/build.log" 2>&1 || { tail -30 "$W/build.log"; exit 1; }
# the console's defaults of THIS tree: entry.cpp's and the Settings start-up's for the picture mode
python3 -I "$SRC/tools/pgo/gen_vita_env.py" --picture-mode "${PICTURE_MODE:-60}" > "$W/vita_env.sh"
. "$W/vita_env.sh"; export EXTRA_ENV="$VITA_ENV"
# a tree the console build or Vita3K has run on is in the Vita's bucket layout, which the harness cannot read: a flat view of links
if [ -n "$(find "$DATA/files" -maxdepth 2 -type d -name '_bk[0-9][0-9]' -print -quit)" ]; then
  [ -d "$W/data-flat" ] || python3 -I "$SRC/tools/pgo/flat_view.py" "$DATA" "$W/data-flat"
  DATA=$W/data-flat
fi
export QEMU_ARM=${QEMU_ARM:-$(command -v qemu-arm || command -v qemu-arm-static || echo qemu-arm)}
mkdir -p "$W/runs"
R="$SRC/tools/pgo/routes"
# route, game seconds (tour5 is the Honeyhive benchmark: it weighs three times an ordinary stage together with tour2; each is 5 s longer
# than before 3.0: the entry screen's step).  run_route.sh exits 1 when no picture was saved (the routes save none): that is normal.
for spec in ${ROUTES:-opening:140 tour:485 tour2:805 tour3:525 tour4:425 tour5:335 tour6:565 tour7:685}; do
  name=${spec%%:*}
  if [ -e "$W/runs/$name" ]; then stamp "route $name: $W/runs/$name exists, kept"; continue; fi
  stamp "route $name (${spec##*:} game s)"
  "$SRC/tools/pgo/run_route.sh" "$W/obj/game" "$DATA" "$R/$name.txt" "$W/runs/$name" "${spec##*:}" || true
  [ -s "$W/runs/$name/route.profraw" ] || echo "route $name wrote no raw profile (see $W/runs/$name/console.log)"
done
raws=(); for f in "$W"/runs/*/route.profraw; do [ -s "$f" ] && raws+=("$f"); done
[ ${#raws[@]} -gt 0 ] || { echo "no raw profile was written"; exit 1; }
stamp "merge ${#raws[@]} raw profile(s)"
llvm-profdata-18 merge --sparse -o "$W/all.profdata" "${raws[@]}"
llvm-profdata-18 merge --text -o "$W/all.txt" "$W/all.profdata"
sed -E -i "s#^$SRC/(decomp|platform|vita)/#/source/\\1/#" "$W/all.txt"
llvm-profdata-18 merge -o "$W/smg.profdata" "$W/all.txt"
rm -f "$W/smg.profdata.gz"; gzip -9 -n -k "$W/smg.profdata"
leaks=$(llvm-profdata-18 merge --text -o - "$W/smg.profdata" | grep -c /home || true)
stamp "profile: $W/smg.profdata(.gz), $leaks line(s) naming /home (must be 0)"
