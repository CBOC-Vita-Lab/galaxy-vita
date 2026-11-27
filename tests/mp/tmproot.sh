# Sourced by the test scripts (tests/mp/*.sh, tools/test_*.sh, tools/mp/partner/test.sh): MP_TMP = a scratch folder private to THIS tree (the
# tree's name and a hash of its path), so two worktrees never share /tmp/<test> binaries or objects (agents got false results from another
# tree's stale objects). Override with MP_TMP=... . Works in sh and bash. Usage, right after `set -...`:
#     . "$(cd "$(dirname "$0")" && pwd)/tmproot.sh"        (a script that is not in tests/mp: ... /../tests/mp/tmproot.sh)
_d=$(cd "$(dirname "$0")" && pwd)
_root=$_d
while [ "$_root" != / ] && [ ! -d "$_root/tests/mp" ]; do _root=$(dirname "$_root"); done
[ "$_root" = / ] && _root=$_d
MP_TMP=${MP_TMP:-/tmp/mp-$(basename "$_root")-$(printf %s "$_root" | md5sum | cut -c1-6)}
mkdir -p "$MP_TMP"
export MP_TMP
unset _d _root
