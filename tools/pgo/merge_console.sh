#!/usr/bin/env bash
# Merge the .profraw files of the instrumented console eboot (README "Record on the console") into the profile the build takes.
#   tools/pgo/merge_console.sh <out-dir> <pgo.profraw>...
# Writes <out-dir>/smg.profdata and smg.profdata.gz.  The functions with internal linkage carry the path of the source file they
# were compiled from: the game units say /source/source/..., the renderer units the resolved checkout (/home/.../source/...).
# Both are rewritten to /source/..., as record_all.sh does, and build.py (prepare_profile) rewrites that to the checkout it builds.
set -euo pipefail
[ $# -ge 2 ] || { sed -n '2,3p' "$0"; exit 2; }
OUT=$1; shift; mkdir -p "$OUT"
PD=$(command -v llvm-profdata-18 || command -v llvm-profdata) || { echo "llvm-profdata not found" >&2; exit 1; }
"$PD" merge --sparse -o "$OUT/all.profdata" "$@"
"$PD" merge --text -o "$OUT/all.txt" "$OUT/all.profdata"
python3 -I - "$OUT/all.txt" <<'PY'
import re, sys
path = sys.argv[1]
rx = re.compile(r"^/.*?(?=/(?:decomp|platform|vita)/)")
with open(path, encoding="utf-8", errors="surrogateescape") as f:
    lines = f.read().split("\n")
n = 0
for i, line in enumerate(lines):
    new = rx.sub("/source", line, count=1)
    if new != line:
        lines[i] = new; n += 1
with open(path, "w", encoding="utf-8", errors="surrogateescape") as f:
    f.write("\n".join(lines))
print(f"{n} function names rewritten to /source/...")
PY
if grep -q '^/home' "$OUT/all.txt"; then echo "a /home path is left in the profile" >&2; exit 1; fi
"$PD" merge -o "$OUT/smg.profdata" "$OUT/all.txt"
gzip -9 -n -c "$OUT/smg.profdata" > "$OUT/smg.profdata.gz"
"$PD" show --detailed-summary "$OUT/smg.profdata" | sed -n '1,12p'
echo "profile: $OUT/smg.profdata.gz"
