#!/bin/bash
# worldsync: the desktop-only harness hooks (SMG_MP_TEST_*, SMG_MP_PLAUS_DIST, SMG_MP_AUTO*) must not be in the Vita objects: the console reads ux0:data/smg/env.txt into
# the environment, so a getenv of such a name there would be a player-reachable switch. Compiles every Vita-built MP source that mentions such a name
# (decomp/src/Game/MP/*.cpp, platform/src/mp/*.cpp: a string cannot be in an object whose source lacks it) for the Vita and greps the object's strings.
# Headers that mention a name are listed as a warning (their includers are then checked by hand).
#   Run through the queue:  tools/buildrun.sh nice -n 10 tests/mp/vita_hook_strings_check.sh [worktree-root]
#   MP_CC=<path of cc.sh> (default /home/claude/g3/tools/cc.sh)
root=$(cd "${1:-$(dirname "$0")/../..}" && pwd)
cc=${MP_CC:-/home/claude/g3/tools/cc.sh}
out=${TMPDIR:-/tmp}/ws_vita_hooks; mkdir -p "$out"
pat='SMG_MP_(TEST_|PLAUS_|AUTO)'
bad=0; n=0
for h in $(cd "$root" && grep -lE "\"$pat" decomp/include/Game/MP/*.hpp decomp/include/Game/MP/*.h platform/src/mp/*.h platform/include/port/mp_*.h 2>/dev/null); do
  echo "WARN: header $h mentions a test hook name: check its includers by hand"
done
for f in $(cd "$root" && grep -lE "\"$pat" decomp/src/Game/MP/*.cpp platform/src/mp/*.cpp 2>/dev/null | sort); do
  o="$out/$(echo "$f" | tr / _).o"; rm -f "$o"; n=$((n+1))
  if ! "$cc" "$root" "$f" vita -c -o "$o" >"$out/$(basename "$f").log" 2>&1 || [ ! -s "$o" ]; then echo "FAIL: $f did not compile for the Vita (see $out/$(basename "$f").log)"; bad=1; continue; fi
  hits=$(strings -a "$o" | grep -E "$pat" | sort -u | tr '\n' ' ')
  if [ -n "$hits" ]; then echo "FAIL: $f (Vita object) holds: $hits"; bad=1; else echo "ok: $f"; fi
  rm -f "$o"
done
echo "vita_hook_strings_check: $n files, $([ $bad = 0 ] && echo PASS || echo FAIL)"
exit $bad
