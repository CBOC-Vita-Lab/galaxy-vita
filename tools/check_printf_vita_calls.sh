#!/usr/bin/env bash
# Fails (exit 1) when a source compiled for the PS Vita uses a printf conversion that the Vita's C library
# does not know: %zu %zd %zx %zX %zi, %td %tu %tx %ti, %jd %ju %jx, %hhd %hhu %hhx.
# Only string literals passed to printf-family calls and the port log helpers are checked.
# Write %u / %d / %x with a static_cast<unsigned> / static_cast<int> instead.
#
# Usage: tools/check_printf_vita.sh [repo-root]   (default: the directory above this script)
set -u
ROOT="${1:-$(cd "$(dirname "$0")/.." && pwd)}"
cd "$ROOT" || exit 2

python3 - "$ROOT" <<'PY'
import os, re, sys

root = sys.argv[1]
# Vita-compiled trees: platform/vgx and vita entirely; platform/src except the desktop-only files.
EXCLUDE_NAMES = {"gl_renderer.cpp", "headless_render.cpp"}
EXCLUDE_DIRS = ("platform/src/xr/",)
SCOPE_DIRS = ("platform/vgx", "vita", "platform/src")
EXTS = (".cpp", ".h", ".hpp", ".inc", ".c")

BAD = re.compile(r"%[-+ #0]*(?:\d+|\*)?(?:\.(?:\d+|\*))?(?:z|t|j|hh)[diouxX]")
CALL = re.compile(r"\b(\w*printf|port_log\w*|port_fatal|StageLog|VGX_LOG\w*|Log)\s*\(")
STR = re.compile(r'"(?:[^"\\\n]|\\.)*"')

def files():
    for d in SCOPE_DIRS:
        base = os.path.join(root, d)
        for dp, _, fns in os.walk(base):
            for fn in fns:
                if not fn.endswith(EXTS) or fn in EXCLUDE_NAMES:
                    continue
                p = os.path.join(dp, fn)
                rel = os.path.relpath(p, root)
                if any(rel.startswith(x) for x in EXCLUDE_DIRS):
                    continue
                yield rel, p
    for fn in sorted(os.listdir(os.path.join(root, "decomp/src/Game/System"))) if os.path.isdir(os.path.join(root, "decomp/src/Game/System")) else []:
        if fn.startswith("Port") and fn.endswith((".cpp", ".h", ".hpp")):
            yield os.path.join("decomp/src/Game/System", fn), os.path.join(root, "decomp/src/Game/System", fn)

def call_span(text, start):
    """Text from the opening paren of a call to its matching close paren (string literals skipped)."""
    depth, i, n = 0, start, len(text)
    while i < n:
        c = text[i]
        if c == '"':
            m = STR.match(text, i)
            if m:
                i = m.end()
                continue
        if c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
            if depth == 0:
                return start, i + 1
        i += 1
    return start, min(n, start + 800)

hits = []
for rel, p in files():
    try:
        text = open(p, encoding="utf-8", errors="replace").read()
    except OSError:
        continue
    for m in CALL.finditer(text):
        s, e = call_span(text, m.end() - 1)
        span = text[s:e]
        for lit in STR.finditer(span):
            body = lit.group(0).replace("%%", "")
            bm = BAD.search(body)
            if bm:
                line = text.count("\n", 0, s + lit.start()) + 1
                hits.append(f"{rel}:{line}: {bm.group(0)} in {lit.group(0)[:90]}")

# Keep one entry per file:line (nested calls can see the same literal twice).
seen, out = set(), []
for h in hits:
    key = h.split(" in ")[0]
    if key not in seen:
        seen.add(key)
        out.append(h)

if out:
    print("printf conversions the Vita C library does not support (use %u/%d/%x with a cast):")
    for h in out:
        print("  " + h)
    print(f"FAILED {len(out)} hit(s)")
    sys.exit(1)
print("check_printf_vita: no unsupported conversions in the Vita sources")
PY
