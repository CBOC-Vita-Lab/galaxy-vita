#!/bin/sh
# Build and run the keepsafe host tests (platform/src/mp/mp_keepsafe.cpp): keepsafe_test (ASan + UBSan) and keepsafe_cut_test (the power-cut
# model: no sanitizer, it interposes the file system calls). Run from the repository root.   tests/mp/keepsafe_build.sh [build]
# clang++ checks every file with -Werror; the binaries are built with g++ (this container's clang has no sanitizer runtime). Objects are kept
# in $KS_OUT.d and rebuilt only when a source or a header changed. Needs a real GameData.bin to build Team Files from (only ever read):
# exit 77 = skipped. KS_ONLY=<test name part>; KS_CUT_ONLY=<operation name part>.
set -eo pipefail
. "$(cd "$(dirname "$0")" && pwd)/tmproot.sh"
CXX=${CXX:-g++}
root=$(cd "$(dirname "$0")/../.." && pwd)
cd "$root"
mod="platform/src/mp/mp_keepsafe.cpp platform/src/mp/mp_savefile.cpp platform/src/mp/mp_savecheck.cpp"
real="platform/src/mp/mp_config.cpp platform/src/mp/mp_ui_flow.cpp platform/src/mp/mp_core_sys.cpp platform/src/mp/mp_core_vita.cpp"
inc="-Iplatform/include -Iplatform/src/mp -Idecomp/include"
hdr="platform/include/port/mp_keepsafe.h platform/src/mp/mp_savefile.h platform/src/mp/mp_config.h tests/mp/keepsafe_world.h tests/mp/keepsafe_build.sh"
out=${KS_OUT:-$MP_TMP/keepsafe_test}
# Objects are reused only for THIS tree at THIS commit: mtimes are meaningless across worktrees / checkouts (a stale object from another tree
# linked into the test made it pass or fail on code that is not here). A stamp file records tree path + HEAD; any difference wipes the objects.
ks_head=$(git -C "$root" rev-parse HEAD 2>/dev/null || echo nogit)
ks_dirty=$( { git -C "$root" diff --stat HEAD -- platform/src/mp platform/include tests/mp 2>/dev/null || true; } | md5sum | cut -c1-12)
plain_flags="-O1 -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0"   # (no fortified fprintf/open: every file-changing call keeps the name the cut test interposes)
stamp="$root|$ks_head|$ks_dirty|$plain_flags"
if [ "$(cat "$out.d/stamp" 2>/dev/null)" != "$stamp" ]; then rm -rf "$out.d"; fi
mkdir -p "$out.d/san" "$out.d/plain"
printf '%s\n' "$stamp" > "$out.d/stamp"
stale() { [ -f "$2" ] || return 0; for h in "$1" $hdr; do [ "$h" -nt "$2" ] && return 0; done; return 1; }
build() {   # $1 kind (san|plain), $2 flags, sources...
    kind=$1; flags=$2; shift 2
    for f in "$@"; do
        o="$out.d/$kind/$(basename "$f").o"
        if stale "$f" "$o"; then
            [ "$kind" = san ] && clang++ -std=c++20 -Wall -Wextra -Werror $inc -fsyntax-only "$f"
            $CXX -std=c++20 $flags -w $inc -c "$f" -o "$o.tmp" && mv "$o.tmp" "$o"
        fi
    done
}
build san "-O0 -fsanitize=address,undefined -fno-sanitize-recover=all" tests/mp/keepsafe_test.cpp platform/src/mp/mp_boot.cpp $mod $real &
b1=$!
build plain "$plain_flags" tests/mp/keepsafe_cut_test.cpp $mod $real &
b2=$!
# (a bare `wait` returns 0 whatever the jobs did: a failed compile then linked the OLD object)
wait $b1 || { echo "keepsafe_build: compile of the sanitizer set FAILED"; exit 97; }
wait $b2 || { echo "keepsafe_build: compile of the cut set FAILED"; exit 97; }
# SYMBOL AUDIT: the power-cut model cuts only at the file-changing calls keepsafe_cut_test.cpp interposes; a product object that imports another
# one (fsync, ftruncate, fprintf ... ) changed the disk with no cut point in front of it. That is a failed build, not a passing test.
python3 tests/mp/cut_audit.py --test "$out.d/plain/keepsafe_cut_test.cpp.o" $(for f in $mod $real; do printf '%s ' "$out.d/plain/$(basename "$f").o"; done) || exit 98
o() { for f in "$@"; do printf '%s ' "$out.d/$2/$(basename "$f").o"; done; }
$CXX -fsanitize=address,undefined $(for f in tests/mp/keepsafe_test.cpp platform/src/mp/mp_boot.cpp $mod $real; do printf '%s ' "$out.d/san/$(basename $f).o"; done) -pthread -Wl,--wrap=_Z14mp_config_holdb -ldl -o "$out"
$CXX $(for f in tests/mp/keepsafe_cut_test.cpp $mod $real; do printf '%s ' "$out.d/plain/$(basename $f).o"; done) -pthread -ldl -o "$out.cut"
[ "$1" = build ] && exit 0
# the two programs are independent (each makes its own worlds in /dev/shm): run side by side, each output kept whole
"$out" > "$out.log" 2>&1 & p1=$!
"$out.cut" > "$out.cut.log" 2>&1 & p2=$!
rc=0
wait $p1 || rc=$?
wait $p2 || rc=$?
{ grep -v '^   [A-Za-z]*: [0-9]* checks, 0 failed$' "$out.log" | grep -v '^== '; } || true   # (display only: no leftover line is grep's 1)
cat "$out.cut.log"
exit $rc
