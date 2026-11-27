#!/bin/bash
# speed31 G: with every new switch at 0 (escalate=false: no escalation, no ownership, no snap) the decisions are the base's. Builds tests/speed_guard_test.cpp against the base headers
# (git show <base>:platform/include/port/*.h, with the lines that name new config fields removed) and against the work tree's, and compares the DECISION-HASH of every scenario.
set -e
BASE=${1:-b0f2d43}
cd "$(dirname "$0")/.."
D=$(mktemp -d)
mkdir -p $D/inc/port
for f in speed_guard.h frame_pace.h; do git show $BASE:platform/include/port/$f > $D/inc/port/$f; done
for f in $(git ls-tree --name-only $BASE platform/include/port/ | xargs -n1 basename); do [ -f $D/inc/port/$f ] || git show $BASE:platform/include/port/$f > $D/inc/port/$f; done
sed '/c\.escalate = false/s/c\.escalate = false;//' tests/speed_guard_test.cpp > $D/base_test.cpp
nice g++ -std=c++17 -I$D/inc -Itests $D/base_test.cpp -o $D/base_test
nice g++ -std=c++17 -Iplatform/include tests/speed_guard_test.cpp -o $D/new_test
B=$($D/base_test | grep DECISION-HASH); N=$($D/new_test | grep DECISION-HASH)
echo "base: $B"; echo "new (switches 0): $N"
[ "$B" = "$N" ] && echo "OFFHASH IDENTICAL" || { echo "OFFHASH DIFFERENT"; exit 1; }
