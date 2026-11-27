#!/usr/bin/env bash
# C182 / C183 first-visit queue (vgx_stage_first_visit.h) and, seedrun31 A3, the data rule that replaced the built-in priority hashes:
#   bash tools/test_c182_firstvisit.sh [VPK | vgx_stagewarm.bin]
# 1. the header's cases; 2. tools/seedrun/stagetags.py selftest; 3. replay on a synthetic list shaped like the C181 test19 one (the nine historical records at
# Observatory 648-651, 669, 671 of 697 and Honeyhive 497-499 of 499): for every stage, the queue the rule until seedrun31 made (built-in hashes and player
# seed) equals the one made from the list tagged by stagetags.py legacy (data alone); stagetags check accepts the tagged list and refuses the untagged one;
# 4. the same on a real packaged list when given: a VPK (tagged through `stagetags.py vpk`, the packaging path; only that member may change) or the file.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
"${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Werror -I"$root/platform/vgx/src" "$root/tests/c182_firstvisit.cpp" -o "$tmp/firstvisit"
"$tmp/firstvisit"
python3 -I "$root/tools/seedrun/stagetags.py" selftest
python3 -I "$root/tools/seedrun/stagetags.py" synth "$tmp/synth.bin" >/dev/null
python3 -I "$root/tools/seedrun/stagetags.py" legacy "$tmp/synth.bin" "$tmp/synth-tagged.bin"
"$tmp/firstvisit" "$tmp/synth.bin" "$tmp/synth-tagged.bin"
python3 -I "$root/tools/seedrun/stagetags.py" check "$tmp/synth-tagged.bin"
if python3 -I "$root/tools/seedrun/stagetags.py" check "$tmp/synth.bin" >/dev/null; then echo "FAIL: an untagged list passes stagetags check"; exit 1; fi
if [[ $# == 1 ]]; then
    case "$1" in
        *.vpk)
            unzip -p "$1" 'shadercache/vgx_stagewarm.bin' > "$tmp/stage.bin"
            # (seedrun31b) the packaging path: the VPK rewritten by `stagetags.py vpk` carries the same tagged list, every other member unchanged
            python3 -I "$root/tools/seedrun/stagetags.py" vpk "$1" "$tmp/tagged.vpk"
            unzip -p "$tmp/tagged.vpk" 'shadercache/vgx_stagewarm.bin' > "$tmp/stage-tagged.bin"
            python3 -I - "$1" "$tmp/tagged.vpk" <<'PY'
import sys, zipfile
a, b = (zipfile.ZipFile(p) for p in sys.argv[1:3])
na, nb = [i.filename for i in a.infolist()], [i.filename for i in b.infolist()]
assert na == nb, "member list differs"
diff = [n for n in na if a.read(n) != b.read(n)]
assert diff == ["shadercache/vgx_stagewarm.bin"], diff
print(f"vpk: {len(na)} members, only shadercache/vgx_stagewarm.bin changed")
PY
            ;;
        *) cp "$1" "$tmp/stage.bin"; python3 -I "$root/tools/seedrun/stagetags.py" legacy "$tmp/stage.bin" "$tmp/stage-tagged.bin" ;;
    esac
    python3 -I "$root/tools/seedrun/stagetags.py" check "$tmp/stage-tagged.bin"
    "$tmp/firstvisit" "$tmp/stage.bin" "$tmp/stage-tagged.bin"
fi
