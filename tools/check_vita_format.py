#!/usr/bin/env python3
"""Fails (exit 1) when a source that runs on the PS Vita has a string literal with a printf conversion the Vita's C library does not know:
%z* (zu zd zx zi), %j* (jd ju jx), %t* (td tu tx), %hh* (hhd hhu hhx). The Vita libc prints them literally ("zu") and does not consume the
argument, so every later argument shifts (a %s after it can crash). Write %u / %d with an explicit cast, or %llu with (unsigned long long).

Scope (decided per tree, see SCOPE / SKIP below):
  scanned : decomp/src, decomp/include, platform/src, platform/include, platform/vgx, vita, net/src, net/include
  skipped : platform/src/xr (OpenXR / Quest build only), platform/src/gx/gl_renderer.cpp and headless_render.cpp (legacy GLES 3.2 / EGL
            renderers, excluded from the Vita runtime by tools/vgx_build/vita_runtime_gen_vgx.py), platform/android, platform/desktop,
            platform/vgx/host (EGL host harness), tests/, net/tests/, tools/ (host-only programs), server/, store/.
Every string literal in a scanned file is checked (not only literals inside printf-like calls), because log wrappers, table-driven formats
and adjacent-literal concatenation ("%" "zu") all reach a printf. Comments are ignored. Adjacent literals are joined before checking.

Usage: tools/check_vita_format.py [repo-root]
"""
import os, re, sys

ROOT = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 else os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SCOPE = ("decomp/src", "decomp/include", "platform/src", "platform/include", "platform/vgx", "vita", "net/src", "net/include")
SKIP_PREFIX = ("platform/src/xr/", "platform/android/", "platform/desktop/", "platform/vgx/host/", "platform/vgx/third_party/")
SKIP_FILES = ("platform/src/gx/gl_renderer.cpp", "platform/src/gx/headless_render.cpp")
EXT = (".c", ".cc", ".cpp", ".h", ".hpp", ".inc", ".inl")
BAD = re.compile(r"(?<!%)(?:%%)*%[-+ #0-9.*]*(?:hh|z|t|j)[diouxXeEfgGaAcspn]")


def literal_runs(src):
    """yield (line, joined_text) per run of adjacent string literals; comments and char literals skipped."""
    i, n, line = 0, len(src), 1
    run, run_line = [], 0
    def flush():
        nonlocal run
        if run:
            val = (run_line, "".join(run))
            run = []
            return val
        return None
    while i < n:
        c = src[i]
        if c == "\n":
            line += 1; i += 1
        elif c in " \t\r\\":
            i += 1
        elif src.startswith("//", i):
            j = src.find("\n", i); i = n if j < 0 else j
        elif src.startswith("/*", i):
            j = src.find("*/", i + 2); j = n if j < 0 else j + 2
            line += src.count("\n", i, j); i = j
        elif c == '"':
            raw = i > 0 and src[i - 1] == "R"
            if raw:   # R"delim( ... )delim"
                m = re.compile(r'"([^(\s]{0,16})\(').match(src, i)
                if m:
                    end = src.find(")" + m.group(1) + '"', m.end())
                    end = n if end < 0 else end
                    body = src[m.end():end]
                    if not run: run_line = line
                    run.append(body); line += body.count("\n"); i = end + len(m.group(1)) + 2
                    continue
            j = i + 1
            while j < n and src[j] != '"' and src[j] != "\n":
                j += 2 if src[j] == "\\" else 1
            if not run: run_line = line
            run.append(src[i + 1:j]); i = j + 1
        elif c == "'":
            j = i + 1
            while j < n and src[j] != "'" and src[j] != "\n":
                j += 2 if src[j] == "\\" else 1
            f = flush()
            if f: yield f
            i = j + 1
        else:
            f = flush()
            if f: yield f
            i += 1
    f = flush()
    if f: yield f


def main():
    bad = []
    nfiles = 0
    for d in SCOPE:
        for dp, _, fns in os.walk(os.path.join(ROOT, d)):
            for fn in fns:
                if not fn.endswith(EXT): continue
                p = os.path.join(dp, fn); rel = os.path.relpath(p, ROOT).replace(os.sep, "/")
                if rel.startswith(SKIP_PREFIX) or rel in SKIP_FILES: continue
                try: src = open(p, encoding="utf-8", errors="replace").read()
                except OSError: continue
                nfiles += 1
                for line, text in literal_runs(src):
                    m = BAD.search(text)
                    if m:
                        bad.append("%s:%d: '%%%s' in \"%s\"" % (rel, line, m.group(0).split("%")[-1], text[:100]))
    if bad:
        print("printf conversions the Vita C library does not know (use %u/%d/%llu with a cast):")
        for b in bad: print("  " + b)
        print("check_vita_format: FAILED, %d hit(s) in %d files scanned" % (len(bad), nfiles))
        return 1
    print("check_vita_format: ok (%d files scanned)" % nfiles)
    return 0


if __name__ == "__main__":
    sys.exit(main())
