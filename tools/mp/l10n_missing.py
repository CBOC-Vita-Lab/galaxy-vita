#!/usr/bin/env python3
"""List the message ids that have an English row in decomp/src/Game/MP/MpMessages.cpp but no German / Italian row in
decomp/src/Game/MP/MpMessagesExtra.cpp (and rows there whose id no longer exists).  Exit 1 when something is missing.
Usage: tools/mp/l10n_missing.py [--verbose]   (run from anywhere; paths follow this file)
"""
import os, re, sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
MAIN = os.path.join(ROOT, "decomp/src/Game/MP/MpMessages.cpp")
EXTRA = os.path.join(ROOT, "decomp/src/Game/MP/MpMessagesExtra.cpp")
ROW = re.compile(r"^\s*\{(MP_T_[A-Z0-9_]+),", re.M)


def ids(path, start=None):
    s = open(path, encoding="utf-8").read()
    if start:
        s = s[s.index(start):]
    return ROW.findall(s)


def main():
    verbose = "--verbose" in sys.argv
    en = ids(MAIN, "const Entry kEntries[]")
    ex = ids(EXTRA, "const Extra kExtra[]")
    # English text of each missing row, for the translator
    text = {}
    src = open(MAIN, encoding="utf-8").read()
    for m in re.finditer(r"\{(MP_T_[A-Z0-9_]+),\s*(?:nullptr|\"[A-Z_]+\"),\s*\{\s*((?:u?\"(?:[^\"\\]|\\.)*\"|[A-Z0-9_]+|\s)+?)\s*,\s*u?\"", src):
        text.setdefault(m.group(1), m.group(2).strip())
    missing = [i for i in en if i not in set(ex)]
    stale = [i for i in ex if i not in set(en)]
    dups = sorted({i for i in ex if ex.count(i) > 1})
    print("English rows: %d, German/Italian rows: %d" % (len(en), len(ex)))
    for i in missing:
        print("MISSING  %s%s" % (i, ("   EN: " + text[i]) if i in text else ""))
    for i in stale:
        print("STALE    %s (in MpMessagesExtra.cpp only)" % i)
    for i in dups:
        print("DUPLICATE %s" % i)
    print("missing: %d, stale: %d, duplicate: %d" % (len(missing), len(stale), len(dups)))
    return 1 if (missing or stale or dups) else 0


if __name__ == "__main__":
    sys.exit(main())
