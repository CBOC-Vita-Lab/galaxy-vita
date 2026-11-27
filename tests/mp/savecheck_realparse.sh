#!/bin/bash
# The game's real save parsers on mutated blocks, with guard pages (owner: snapval). See savecheck_realparse.cpp.
#   tests/mp/savecheck_realparse.sh [iterations]
# Builds two programs with the game's desktop flags (tools/cc.sh): one linked with this worktree's (hardened) parser objects,
# one with the original objects of the baseline desktop build (read only). Needs /data/cooked (ScenarioData) and the test save
# (copied to a temp dir; never written).
set -eo pipefail
. "$(cd "$(dirname "$0")" && pwd)/tmproot.sh"
root=$(cd "$(dirname "$0")/../.." && pwd)
out=${MP_TEST_OUT:-$MP_TMP/mp_savecheck}/realparse
base=${SMG_DESK_BUILD:-/home/claude/g3/desk/build}/CMakeFiles/decomp.dir/decomp/src
cc=/home/claude/g3/tools/cc.sh
save=${SAVECHECK_SAVE:-/data/saves0/saves/title/00010000/524d4750/data/GameData.bin}
mkdir -p "$out"
: > "$out/build.log"   # (the game headers' warnings)
python3 -I "$root/tests/mp/savecheck_scenarios.py" "${SMG_COOKED:-/data/cooked}" > "$out/scenarios.inc"
tmp=$(mktemp -d $MP_TMP/savecheck-real-XXXXXX)
cp "$save" "$tmp/GameData.bin"
for kind in new old; do
  def=""; [ $kind = new ] && def="-DSAVECHECK_NEW=1"
  nice -n 10 $cc "$root" tests/mp/savecheck_realparse.cpp desk "-DSAVECHECK_SCENARIOS=\"$out/scenarios.inc\"" $def -O1 -g -c -o "$out/main-$kind.o" 2>> "$out/build.log"
done
objs_new=""
for f in BinaryDataChunkHolder GameDataGalaxyStorage SpinDriverPathStorage StarPieceAlmsStorage; do
  nice -n 10 $cc "$root" decomp/src/Game/System/$f.cpp desk -c -o "$out/$f.o" 2>> "$out/build.log"
  objs_new="$objs_new $out/$f.o"
done
objs_old=""
for f in BinaryDataChunkHolder GameDataGalaxyStorage SpinDriverPathStorage StarPieceAlmsStorage; do objs_old="$objs_old $base/Game/System/$f.cpp.o"; done
common="$base/Game/System/BinaryDataContentAccessor.cpp.o $base/Game/Util/HashUtil.cpp.o $base/Game/Util/MemoryUtil.cpp.o
 $base/Game/Util/MathUtil.cpp.o $base/JSystem/JSupport/JSUMemoryStream.cpp.o $base/JSystem/JSupport/JSUInputStream.cpp.o
 $base/JSystem/JSupport/JSUOutputStream.cpp.o"
link="clang++ -stdlib=libc++ -no-pie -Wl,--unresolved-symbols=ignore-all"
$link -o "$out/realparse-new" "$out/main-new.o" $objs_new $common
$link -o "$out/realparse-old" "$out/main-old.o" $objs_old $common
echo "built in $out"
[ "$1" = "build" ] && exit 0
"$out/realparse-new" "$tmp/GameData.bin" "${1:-200000}" | tee "$out/new.txt"
"$out/realparse-old" "$tmp/GameData.bin" "${1:-200000}" | tee "$out/old.txt"
# valid data: the hardened parsers end in exactly the state of the original ones
if diff <(grep '^state' "$out/new.txt") <(grep '^state' "$out/old.txt") > /dev/null; then echo "valid blocks: same state as the original parsers"; else echo "valid blocks: STATE DIFFERS"; exit 1; fi
cmp -s "$save" "$tmp/GameData.bin" && echo "test save unchanged"
rm -rf "$tmp"
