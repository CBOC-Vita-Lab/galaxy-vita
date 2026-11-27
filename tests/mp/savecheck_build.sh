#!/bin/bash
# Build and run the save block validator tests (ASan + UBSan; owner: snapval).
#   tests/mp/savecheck_build.sh                 build, run savecheck_test (1M fuzz) and save_test
#   tests/mp/savecheck_build.sh build           build only
#   SAVECHECK_FUZZ=N tests/mp/savecheck_build.sh  fuzz N snapshots instead of 1M
# The test save (/data/saves0/saves/.../GameData.bin) is only read (copied to a temp dir); the cooked game data (/data/cooked) is
# only read (ScenarioData for the star masks; without it the mask checks are skipped).
set -eo pipefail
. "$(cd "$(dirname "$0")" && pwd)/tmproot.sh"
root=$(cd "$(dirname "$0")/../.." && pwd)
out=${MP_TEST_OUT:-$MP_TMP/mp_savecheck}
mkdir -p "$out"
# (this machine's clang has no sanitizer runtime: g++ when clang cannot link -fsanitize)
if [ -z "$CXX" ]; then
  if echo 'int main(){}' | clang++ -fsanitize=address -x c++ - -o $MP_TMP/.mp_asan_probe 2>/dev/null; then CXX=clang++; else CXX=g++; fi
fi
FLAGS="-std=c++20 -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -I$root/platform/include"
scen=""
if [ -d "${SMG_COOKED:-/data/cooked}/files/StageData" ]; then
  python3 -I "$root/tests/mp/savecheck_scenarios.py" "${SMG_COOKED:-/data/cooked}" > "$out/savecheck_scenarios.inc"
  scen="-DSAVECHECK_SCENARIOS=\"$out/savecheck_scenarios.inc\""
fi
SRCS="$root/platform/src/mp/mp_savecheck.cpp $root/platform/src/mp/mp_savefile.cpp"
nice -n 10 $CXX $FLAGS $scen -o "$out/savecheck_test" "$root/tests/mp/savecheck_test.cpp" $SRCS -lpthread
nice -n 10 $CXX $FLAGS -o "$out/save_test" "$root/tests/mp/save_test.cpp" $SRCS -lpthread
nice -n 10 $CXX $FLAGS -I$root/decomp/include -o "$out/savegate_test" "$root/tests/mp/savegate_test.cpp" $SRCS -lpthread
nice -n 10 $CXX $FLAGS -o "$out/slot_state_test" "$root/tests/mp/slot_state_test.cpp" $SRCS -lpthread   # flow (review 2 #3)
echo "built in $out"
[ "$1" = "build" ] && exit 0
nice -n 10 "$out/savecheck_test" --fuzz "${SAVECHECK_FUZZ:-1000000}"
nice -n 10 "$out/save_test"
nice -n 10 "$out/savegate_test"
nice -n 10 "$out/slot_state_test"
