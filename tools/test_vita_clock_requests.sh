#!/usr/bin/env bash
# Compile the actual entry clock block against host power-call/log mocks.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
python3 - "$root/vita/vgx-game/entry.cpp" "$out/vita_clock_block.inc" <<'PY'
from pathlib import Path
import sys
source = Path(sys.argv[1]).read_text()
needle = 'const char* e = std::getenv("GXV_VITA_CLOCKS")'
assert source.count(needle) == 1, 'one owning clock block required'
at = source.index(needle)
begin = source.rfind('\n    {', 0, at)
end = source.index('\n    if (const char* mb = std::getenv("PETARI_MEMBENCH")', at)
assert begin >= 0 and end > at, 'clock block boundaries required'
block = source[begin + 1:end]
assert block.startswith('    {\n') and block.rstrip().endswith('}'), 'complete owning block required'
Path(sys.argv[2]).write_text(block)
PY
for diagnostics in 0 1; do
  "${CXX:-clang++}" -std=c++17 -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-omit-frame-pointer \
    -DPORT_RUNTIME_DIAGNOSTICS="$diagnostics" -I"$out" "$root/tests/vita_clock_requests.cpp" -o "$out/clocks-$diagnostics"
  UBSAN_OPTIONS=halt_on_error=1 "$out/clocks-$diagnostics"
done
