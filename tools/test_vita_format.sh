#!/usr/bin/env bash
# Host test for tools/check_vita_format.py (the guard against %z / %j / %t / %hh in code that runs on the Vita).
# 1. the repository itself must be clean; 2. the checker must catch each spelling on a scratch tree and must leave the host-only paths alone.
set -u
root=$(cd "$(dirname "$0")/.." && pwd)
chk="$root/tools/check_vita_format.py"
fail=0
python3 -I "$chk" "$root" || { echo "FAIL: the repository has a %z/%j/%t/%hh format in Vita code"; fail=1; }
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
mk() { mkdir -p "$tmp/$1/$(dirname "$2")"; printf '%s\n' "$3" > "$tmp/$1/$2"; }
expect() {   # name rc path source
  local t="$tmp/$1"; python3 -I "$chk" "$t" > "$tmp/$1.out" 2>&1; local rc=$?
  if [ "$rc" != "$2" ]; then echo "FAIL: $1 expected rc $2, got $rc"; cat "$tmp/$1.out"; fail=1; fi
}
mk zu    platform/src/mp/a.cpp   'void f(size_t n){ port_log("n=%zu", n); }';              expect zu 1
mk zd    vita/runtime/a.cpp      'void f(){ printf("%5zd", (size_t)1); }';                   expect zd 1
mk ju    decomp/src/Game/MP/a.cpp 'void f(){ mp_log("%ju", 1); }';                           expect ju 1
mk td    net/src/a.cpp           'void f(){ snprintf(b, 8, "%td", p - q); }';                expect td 1
mk hh    platform/vgx/src/a.cpp  'void f(){ Log("%hhu", 1); }';                              expect hh 1
mk split platform/src/port/a.cpp 'void f(){ port_log("%" "zu", n); }';                        expect split 1
mk table decomp/src/Game/Util/a.cpp 'static const char* kFmt = "id %zu";';                    expect table 1
mk ok1   platform/src/mp/a.cpp   'void f(){ port_log("100%%zu is text, %u %llu", 1u, 2ull); } // "%zu" in a comment'; expect ok1 0
mk ok2   platform/src/xr/a.cpp   'void f(){ port_log("%zu", n); }';                           expect ok2 0
mk ok3   platform/src/gx/gl_renderer.cpp 'void f(){ port_log("%zu", n); }';                  expect ok3 0
mk ok4   tests/a.cpp             'void f(){ printf("%zu", n); }';                            expect ok4 0
[ "$fail" = 0 ] && echo "test_vita_format: PASS" || { echo "test_vita_format: FAIL"; exit 1; }
