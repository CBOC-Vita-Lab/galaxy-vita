#!/usr/bin/env python3
"""Strip `L` / `UL` / `LU` suffixes from integer literals in decomp code.
CodeWarrior's long is 32 bits, so `3L` == `3` there; on LP64 the suffix makes
the literal 64-bit and turns many overload calls ambiguous."""
import os
import re
import sys

EXTS = ('.c', '.cpp', '.h', '.hpp', '.inc')
LIT = re.compile(r'(?<![\w.])((?:0[xX][0-9a-fA-F]+)|(?:\d+))(?:([uU])[lL]|[lL]([uU])?)(?![\w])')


def fix_code(code):
    return LIT.sub(lambda m: m.group(1) + ((m.group(2) or m.group(3)) or ''), code)


def process(text):
    out = []
    i = 0
    n = len(text)
    cs = 0
    while i < n:
        ch = text[i]
        if ch == '/' and i + 1 < n and text[i + 1] in '/*':
            out.append(fix_code(text[cs:i]))
            if text[i + 1] == '/':
                j = text.find('\n', i)
                j = n if j < 0 else j
            else:
                j = text.find('*/', i + 2)
                j = n if j < 0 else j + 2
            out.append(text[i:j])
            i = cs = j
            continue
        if ch in '"\'':
            out.append(fix_code(text[cs:i]))
            j = i + 1
            while j < n and text[j] != ch and text[j] != '\n':
                j += 2 if text[j] == '\\' else 1
            j = min(j + 1, n)
            out.append(text[i:j])
            i = cs = j
            continue
        i += 1
    out.append(fix_code(text[cs:]))
    return ''.join(out)


def main():
    changed = 0
    for root in sys.argv[1:]:
        for dp, _, files in os.walk(root):
            for f in files:
                if not f.endswith(EXTS):
                    continue
                p = os.path.join(dp, f)
                with open(p, 'rb') as fh:
                    t = fh.read().decode('utf-8', 'surrogateescape')
                nt = process(t)
                if nt != t:
                    with open(p, 'wb') as fh:
                        fh.write(nt.encode('utf-8', 'surrogateescape'))
                    changed += 1
    print('changed', changed)


if __name__ == '__main__':
    main()
