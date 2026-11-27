#!/bin/sh
# savesus: the host tests that settle the devil2 suspicions S1 S2 S6 S7 S8 (see the test files; verdicts in savesus REPORT.md).
set -eo pipefail
. "$(cd "$(dirname "$0")" && pwd)/tmproot.sh"
cd "$(dirname "$0")/../.."
out=${OUT:-$MP_TMP/savesus}
mkdir -p "$out"
# S7: emulated TLS, counts the thread-local lookups made by mp_config.cpp
nice -n 10 ${CLANGXX:-clang++} -std=c++20 -femulated-tls -g -O1 -Iplatform/include -Iplatform/src/mp -Idecomp/include tests/mp/s7_tls_test.cpp \
  platform/src/mp/mp_config.cpp platform/src/mp/mp_core_sys.cpp platform/src/mp/mp_core_vita.cpp -pthread -Wl,--wrap=__emutls_get_address -o "$out/s7"
nice -n 10 "$out/s7"
# S8: the recovery prune (real mp_savefile.cpp, ASan+UBSan)
nice -n 10 g++ -std=c++20 -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined -Iplatform/include tests/mp/s8_recovery_prune_test.cpp \
  platform/src/mp/mp_savecheck.cpp platform/src/mp/mp_savefile.cpp -pthread -o "$out/s8"
ASAN_OPTIONS=detect_leaks=0 nice -n 10 "$out/s8"
# S6: nand.cpp's atomic-write flag across Create / Open / Close (ASan+UBSan, then ThreadSanitizer)
S6SRC="tests/mp/s6_nand_flag_test.cpp platform/src/nand/nand.cpp platform/src/mp/mp_savefile.cpp platform/src/mp/mp_savecheck.cpp"
S6INC="-Itests/mp/savelife/fake -Idecomp/include -Iplatform/include -Idecomp/libs/RVL_SDK/include -Iplatform/src/mp"
nice -n 10 g++ -std=c++20 -g -O1 -fsanitize=address,undefined -fpermissive -w $S6INC $S6SRC -pthread -o "$out/s6"
ASAN_OPTIONS=detect_leaks=0 nice -n 10 "$out/s6"
nice -n 10 g++ -std=c++20 -g -O1 -fsanitize=thread -fpermissive -w $S6INC $S6SRC -pthread -o "$out/s6t"
TSAN_OPTIONS="halt_on_error=1 exitcode=66" nice -n 10 "$out/s6t" tsan
# S1: guest STAR claim validation (pure logic; the rule of SAVESUS-FIXES.md is applied by glitchfix)
nice -n 10 g++ -std=c++20 -g -O1 -fsanitize=address,undefined -Idecomp/include -Iplatform/include tests/mp/s1_star_claim_test.cpp -o "$out/s1"
nice -n 10 "$out/s1"
# S4 scenario: the backup before the first "Play on my file" session survives ten more
nice -n 10 g++ -std=c++20 -g -O1 -fsanitize=address,undefined -Iplatform/include tests/mp/s4_backup_golden_test.cpp platform/src/mp/mp_savecheck.cpp platform/src/mp/mp_savefile.cpp -pthread -o "$out/s4"
ASAN_OPTIONS=detect_leaks=0 nice -n 10 "$out/s4"
