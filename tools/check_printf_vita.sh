#!/usr/bin/env bash
# Fails (exit 1) when a printf-family format string in code the Vita build compiles uses %z, %t, %j or %hh: the Vita's C library
# crashes or misprints on them (%ll is fine). Write a cast instead: "%u" with static_cast<unsigned>(size), "%d"/"%ld" for ptrdiff, "%llu"
# for a 64-bit value. Scans string literals only, one line at a time; text after a " //" comment start is ignored.
# Not scanned: tests/, net/tests/, platform/desktop/, and the desktop-only renderers (platform/src/gx, platform/src/xr).
# Usage: tools/check_printf_vita.sh [tree]      (tree default: the repository this script is in)
set -u
self=$(cd "$(dirname "$0")" && pwd)
root=${1:-$(cd "$self/.." && pwd)}
cd "$root" || exit 2
dirs=(decomp/src/Game/MP decomp/include/Game/MP platform/src/mp platform/src/port platform/include net/src net/include vita platform/vgx)
# a literal containing %[flags][width][.prec] then z|t|j|hh then a conversion letter (a doubled %% is skipped)
re='"[^"]*(?<!%)(?:%%)*%[-+ #0-9.*]*(?:hh|z|t|j)[diouxXeEfgGaAcspn]'
found=0
while IFS= read -r hit; do
  file=${hit%%:*}; rest=${hit#*:}; line=${rest%%:*}; text=${rest#*:}
  code=$(printf '%s' "$text" | sed -E 's#(^|[[:space:]])//.*$##')   # (a trailing // comment is not code)
  if printf '%s' "$code" | grep -qP "$re"; then
    printf '%s:%s: %%z/%%t/%%j/%%hh in a format string: %s\n' "$file" "$line" "$(printf '%s' "$code" | sed -E 's/^[[:space:]]+//' | cut -c1-120)"
    found=$((found + 1))
  fi
done < <(grep -rnP --include='*.cpp' --include='*.c' --include='*.cc' --include='*.h' --include='*.hpp' --include='*.inc' "$re" "${dirs[@]}" 2>/dev/null)
# second pass: a conversion split across adjacent string literals ("%" "zu"), which the line-by-line grep above cannot see
split=$(python3 "$self/check_printf_vita_split.py" "$root" "${dirs[@]}") || { printf '%s\n' "$split"; found=$((found + $(printf '%s\n' "$split" | grep -c .))); }
# third pass (merge c99): the performance line's checker, which covers all of platform/src (call-aware: only literals inside printf-family and
# port_log calls). Kept as its own file so the performance chat's script stays byte-identical; skipped when it is not beside this one.
if [ -f "$self/check_printf_vita_calls.sh" ]; then
  bash "$self/check_printf_vita_calls.sh" "$root" || found=$((found + 1))
fi
# fourth pass (fmt31): every literal of every Vita-side tree (decomp/src and include, net, platform minus the desktop-only files), not only the calls the passes above know
if [ -f "$self/check_vita_format.py" ]; then
  python3 -I "$self/check_vita_format.py" "$root" || found=$((found + 1))
fi
if [ "$found" -gt 0 ]; then echo "check_printf_vita: $found problem line(s)"; exit 1; fi
echo "check_printf_vita: ok"
