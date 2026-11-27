#!/bin/sh
# Build and run the puppet demo host tests (decomp/include/Game/MP/MpDemoLogic.hpp, platform/src/mp/mp_demo.cpp).
# ASan + UBSan with g++, clang++ -Werror as a second compiler. Also checks that the demo's own sources write no file.
# Run from the repo root.
set -eo pipefail
. "$(cd "$(dirname "$0")" && pwd)/tmproot.sh"
CXX=${CXX:-g++}
SRC="tests/mp/demo_test.cpp platform/src/mp/mp_demo.cpp"
clang++ -std=c++20 -Wall -Wextra -Werror -I decomp/include -I platform/include -c tests/mp/demo_test.cpp -o $MP_TMP/demo_test_clang.o
clang++ -std=c++20 -Wall -Wextra -Werror -I decomp/include -I platform/include -c platform/src/mp/mp_demo.cpp -o $MP_TMP/mp_demo_clang.o
$CXX -std=c++20 -fsanitize=address,undefined -fno-sanitize-recover=all -g -Wall -Wextra -I decomp/include -I platform/include $SRC -o $MP_TMP/demo_test
$MP_TMP/demo_test
# the demo's own sources open no file for writing and call no save / cfg writer
if grep -nE 'fopen|ofstream|fwrite|open\(|remove\(|rename\(|mp_cfg_save|port_save|SaveWrite|writeFile|mp_entry_remember' decomp/src/Game/MP/MpDemo.cpp platform/src/mp/mp_demo.cpp decomp/include/Game/MP/MpDemoLogic.hpp; then
    echo "FAIL: a file write in the demo sources"; exit 1
fi
echo "demo: no file writes in the demo sources"
# wire: the menu tour walks the one-door gathering screen, not the old lobby (HANDOFF section 8)
if grep -n 'S_LOBBY' decomp/src/Game/MP/MpDemo.cpp; then echo "FAIL: the demo's menu tour still shows the old lobby"; exit 1; fi
grep -q 'demoShow(MpMenu::S_GATHER)' decomp/src/Game/MP/MpDemo.cpp || { echo "FAIL: the demo's menu tour never shows the gathering screen"; exit 1; }
echo "demo: the menu tour shows the gathering screen"
