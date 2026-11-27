#!/bin/bash
# linkfix: weak references in the multiplayer service layer must name the C symbol the definition really has.
# mp_service.cpp declared mp_core_set_own_friend_code weak with C++ linkage while mp_core.cpp defines it extern "C" (port/mp_core_game.h):
# the weak reference was to the mangled name, always null, and the core was never told this console's friend code.
#   tests/mp/linkage_test.sh   (from anywhere)
set -euo pipefail
. "$(cd "$(dirname "$0")" && pwd)/tmproot.sh"
cd "$(dirname "$0")/../.."
OUT=${OUT:-$MP_TMP/mp_linkage_test}; mkdir -p "$OUT"
if [ -f net/include/np_https.h ]; then INC="-Inet/include -Inet/include/online"; else R=/home/claude/g3/ref/strikers/native/platform/sdk/net; INC="-I$R -I$R/online"; fi
fail=0
check_obj() {   # <source> <unmangled name that must be weakly referenced, no mangled twin>
  local src=$1 name=$2 o="$OUT/$(basename "$1").o"
  g++ -std=c++20 -O0 -Iplatform/include -Iplatform/src/mp $INC -c "$src" -o "$o"
  local syms; syms=$(nm --no-demangle "$o")
  if echo "$syms" | grep -Eq "^[ ]*[wv] _Z[0-9]+$name"; then echo "FAIL: $src references the C++-mangled $name"; fail=1
  elif ! echo "$syms" | grep -Eq "^[ ]*[wv] $name\$"; then echo "FAIL: $src has no weak reference to the C symbol $name"; fail=1
  else echo "ok: $src weakly references C symbol $name"; fi
}
check_obj platform/src/mp/mp_service.cpp mp_core_set_own_friend_code
# the definition really is the unmangled symbol (mp_core.cpp: compile-free check on the header and the definition's extern "C" block)
grep -q 'extern "C" {' platform/include/port/mp_core_game.h && awk '/extern "C" \{/{c=1} /mp_core_set_own_friend_code/{if(c){f=1}} END{exit !f}' platform/include/port/mp_core_game.h \
  && echo "ok: port/mp_core_game.h declares mp_core_set_own_friend_code inside extern \"C\"" || { echo "FAIL: header declaration is not in extern \"C\""; fail=1; }
[ $fail = 0 ] && echo "linkage_test: PASS" || { echo "linkage_test: FAIL"; exit 1; }
