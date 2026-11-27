#!/usr/bin/env python3
"""check_env_file.py: lint a ux0:data/smg/env.txt the way the Vita parser (vita/vgx-game/entry.cpp LoadEnvFile) reads it.
The parser: one NAME=value per line; the value is the rest of the line; '#' only starts a comment at the START of a line;
no limit on lines or keys; a line over 2047 bytes is cut and its rest is read as another line; spaces around '=' are NOT removed.
usage: tools/check_env_file.py ENV.txt [...] [--src ROOT]   (--src: also report names no source file reads: typos that do nothing)
Exit 1 on any error. Warnings (W) do not fail."""
import os, re, sys

def known_names(root):
    names = set()
    for sub in ("platform", "vita", "decomp/src", "tools"):
        for dp, dn, fn in os.walk(os.path.join(root, sub)):
            for f in fn:
                if f.endswith((".c", ".cc", ".cpp", ".h", ".hpp", ".inc", ".py", ".sh")):
                    try: txt = open(os.path.join(dp, f), errors="replace").read()
                    except OSError: continue
                    names.update(re.findall(r'\b(?:GXV|PETARI|MSC)_[A-Z0-9_]+', txt))
    return names

def lint(path, known):
    errs = warns = 0
    raw = open(path, "rb").read()
    if raw.startswith(b"\xef\xbb\xbf"):
        print("%s:1: E byte-order mark: the first name becomes '\\xef\\xbb\\xbfNAME' and is lost" % path); errs += 1
    seen = {}
    nset = 0
    for i, ln in enumerate(raw.split(b"\n"), 1):
        if len(ln) >= 2047:
            print("%s:%d: E line is %d bytes (parser buffer 2048): cut, the rest is read as a new line" % (path, i, len(ln))); errs += 1
        s = ln.decode("latin1").lstrip(" \t")
        if not s or s[0] == "#" or s[0] in "\r\n": continue
        if "=" not in s: print("%s:%d: W no '=': ignored (the game counts it in 'env.txt: WARNING ... ignored')" % (path, i)); warns += 1; continue
        name, val = s.split("=", 1)
        val = val.rstrip("\r\n ")
        if not name.strip(" \t"):   # (strk31: setenv("") is refused by the system; the game now counts it in 'env.txt: WARNING ... ignored')
            print("%s:%d: E empty variable name: the line is ignored by the game (setenv refuses it)" % (path, i)); errs += 1; continue
        if name != name.rstrip(" \t") or name.endswith((" ", "\t")):
            print("%s:%d: E space before '=': the variable is called %r" % (path, i, name)); errs += 1
        if val[:1] in (" ", "\t"):
            print("%s:%d: W value starts with a space: %r" % (path, i, val[:12])); warns += 1
        if re.search(r'\s(?:GXV|PETARI|MSC)_[A-Z0-9_]+=', val):
            print("%s:%d: E a second assignment on the same line (the whole rest is the first one's value): %s..." % (path, i, val[:60])); errs += 1
        if "#" in val and not name.startswith(("PETARI_STAGE", "PETARI_FLOW")):
            print("%s:%d: W '#' inside a value is part of the value (a trailing comment is not stripped): %r" % (path, i, val[-30:])); warns += 1
        if name in seen:
            print("%s:%d: W %s set again (line %d): the later one wins" % (path, i, name, seen[name])); warns += 1
        seen[name] = i; nset += 1
        if known is not None and re.match(r'(?:GXV|PETARI|MSC)_', name) and name not in known:
            print("%s:%d: W %s is not read by any source file (typo?)" % (path, i, name)); warns += 1
    print("%s: %d variables, %d errors, %d warnings" % (path, nset, errs, warns))
    return errs

args = [a for a in sys.argv[1:]]
root = None
if "--src" in args:
    k = args.index("--src"); root = args[k + 1]; del args[k:k + 2]
if not args: print(__doc__); sys.exit(2)
known = known_names(root) if root else None
bad = sum(lint(p, known) for p in args)
sys.exit(1 if bad else 0)
