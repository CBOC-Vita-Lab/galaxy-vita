#!/usr/bin/env python3
"""CodeWarrior's `long` is 32 bits; on LP64 hosts it is 64 bits.  Rewrite
`long` -> `int` and `unsigned long` -> `unsigned int` in decomp code (outside
of string literals and comments), keeping `long long` and `long double`."""
import os
import re
import sys

EXTS = ('.c', '.cpp', '.h', '.hpp', '.inc')
PAT = re.compile(r'(?<![\w])(unsigned\s+|signed\s+)?long(?![\w])(?!\s+long\b)(?!\s+double\b)(\s+int(?![\w]))?')


def fix_code(code):
    def repl(m):
        # skip the second word of `long long`
        start = m.start()
        before = code[max(0, start - 6):start]
        if re.search(r'long\s+$', before):
            return m.group(0)
        prefix = m.group(1) or ''
        return prefix + 'int'
    return PAT.sub(repl, code)


def process(text):
    out = []
    i = 0
    n = len(text)
    code_start = 0
    buf = []
    while i < n:
        ch = text[i]
        if ch == '/' and i + 1 < n and text[i + 1] in '/*':
            out.append(fix_code(text[code_start:i]))
            if text[i + 1] == '/':
                j = text.find('\n', i)
                j = n if j < 0 else j
            else:
                j = text.find('*/', i + 2)
                j = n if j < 0 else j + 2
            out.append(text[i:j])
            i = code_start = j
            continue
        if ch in '"\'':
            out.append(fix_code(text[code_start:i]))
            j = i + 1
            while j < n and text[j] != ch and text[j] != '\n':
                j += 2 if text[j] == '\\' else 1
            j = min(j + 1, n)
            out.append(text[i:j])
            i = code_start = j
            continue
        i += 1
    out.append(fix_code(text[code_start:]))
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
                    raw = fh.read()
                text = raw.decode('utf-8', errors='surrogateescape')
                if 'long' not in text:
                    continue
                new = process(text)
                if new != text:
                    with open(p, 'wb') as fh:
                        fh.write(new.encode('utf-8', errors='surrogateescape'))
                    changed += 1
    print('changed', changed, 'files')


if __name__ == '__main__':
    main()
