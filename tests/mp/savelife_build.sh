#!/bin/bash
# savelife (savefix): build and run the save-safety harness (ASan + UBSan). The REAL MpSaveGame.cpp, MpSaveGate.hpp, MpSaveHook.hpp,
# mp_savefile.cpp, mp_savecheck.cpp, mp_config.cpp and nand.cpp against tests/mp/savelife/fakegame.* (see tests/mp/savelife_test.cpp).
#   tests/mp/savelife_build.sh              build, scripted lifecycles, SAVELIFE_SEQS (default 20000) random sequences
#   tests/mp/savelife_build.sh build        build only
#   SAVELIFE_SEQS=N SAVELIFE_STEPS=K SAVELIFE_SEED=S  (CPU-heavy: run through tools/buildrun.sh)
# Reads the test save /data/saves0/saves/.../GameData.bin (copied); writes only under /tmp/savelife.*.
set -eo pipefail
. "$(cd "$(dirname "$0")" && pwd)/tmproot.sh"
root=$(cd "$(dirname "$0")/../.." && pwd)
out=${MP_TEST_OUT:-$MP_TMP/mp_savelife}
src=${SAVELIFE_SRC:-$root}   # (the tree whose save code is tested: another worktree for a before/after comparison)
mkdir -p "$out"
CXX=${CXX:-g++}
WRAP="-Wl,--wrap=open,--wrap=read,--wrap=write,--wrap=fsync,--wrap=rename,--wrap=unlink,--wrap=remove,--wrap=open64,--wrap=mkdir,--wrap=rmdir,--wrap=fwrite,--wrap=fprintf,--wrap=fopen,--wrap=fclose,--wrap=malloc,--wrap=calloc,--wrap=time,--wrap=stat"
FLAGS="-std=c++20 -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 -Wall -Wno-unused-parameter -Wno-unused-function -Wno-unused-variable -Wno-missing-field-initializers -fpermissive"
INC="-I$root/tests/mp/savelife/fake -I$src/decomp/include -I$src/platform/include -I$src/decomp/libs/RVL_SDK/include -I$src/platform/src/mp"
SRCS="$src/decomp/src/Game/MP/MpSaveGame.cpp $src/platform/src/mp/mp_savefile.cpp $src/platform/src/mp/mp_savecheck.cpp $src/platform/src/mp/mp_config.cpp $src/platform/src/nand/nand.cpp"
[ -f "$src/platform/src/mp/mp_chat.cpp" ] && SRCS="$SRCS $src/platform/src/mp/mp_chat.cpp"
# objects first (the symbol audit reads them), then the link. The folder is wiped first: objects of another tree or commit are never reused.
rm -rf "$out/obj"; mkdir -p "$out/obj"
objs=""
for f in "$root/tests/mp/savelife_test.cpp" "$root/tests/mp/savelife/fakegame.cpp" $SRCS; do
  o="$out/obj/$(basename "$f").o"
  nice -n 10 $CXX $FLAGS $INC ${SAVELIFE_DEFS} -c "$f" -o "$o"
  objs="$objs $o"
done
# SYMBOL AUDIT: the cut points are the calls wrapped below; a product object that imports another file-changing libc symbol changes the card with
# no cut point in front of it. That fails the build (tests/mp/cut_audit.py).
python3 "$root/tests/mp/cut_audit.py" --test "$out/obj/fakegame.cpp.o" --wrap "$WRAP" $(for f in $SRCS; do printf '%s ' "$out/obj/$(basename "$f").o"; done) || exit 98
nice -n 10 $CXX $FLAGS $objs -o "$out/savelife_test" -lpthread $WRAP
echo "built $out/savelife_test"
# A7: the boot repair (.mptmp only; a lone .mpbak is not restored) and NANDDelete's side files (nand.cpp against plain stubs, no syscall wrapping)
nice -n 10 $CXX -std=c++20 -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined -fpermissive -w $INC -o "$out/nand_a7_test" \
  "$root/tests/mp/nand_a7_test.cpp" $src/platform/src/nand/nand.cpp $src/platform/src/mp/mp_savefile.cpp $src/platform/src/mp/mp_savecheck.cpp -lpthread
[ "$1" = "build" ] && exit 0
export ASAN_OPTIONS=${ASAN_OPTIONS:-detect_leaks=0}
nice -n 10 "$out/nand_a7_test"
export ASAN_OPTIONS=${ASAN_OPTIONS:-detect_leaks=0}
nice -n 10 "$out/savelife_test" --scripted
nice -n 10 "$out/savelife_test" --random "${SAVELIFE_SEQS:-20000}" --steps "${SAVELIFE_STEPS:-60}" --seed "${SAVELIFE_SEED:-1}"
# Safe saves turned ON in Settings (the model with cuts inside the single-player write): the same scripted lifecycles and a shorter random run
SAVELIFE_SP_ATOMIC=1 nice -n 10 "$out/savelife_test" --scripted
SAVELIFE_SP_ATOMIC=1 nice -n 10 "$out/savelife_test" --random "${SAVELIFE_SEQS_ON:-${SAVELIFE_SEQS:-20000}}" --steps "${SAVELIFE_STEPS:-60}" --seed "${SAVELIFE_SEED:-1}"
