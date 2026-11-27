#!/usr/bin/env python3
"""Second pass of check_printf_vita.sh: a %z / %t / %j / %hh conversion split across ADJACENT string literals ("%" "zu", "%z" "u" on the
next line, a literal, a comment, then the rest). C joins adjacent literals before printf sees them; the line-by-line grep cannot.

  check_printf_vita_split.py ROOT DIR...      exit 1 and one line per hit; 0 when clean.
Comments and character literals are skipped; only joined runs of two or more literals are checked (single literals are the first pass's job).
"""
import os, re, sys

BAD = re.compile(r'(?<!%)(?:%%)*%[-+ #0-9.*]*(?:hh|z|t|j)[diouxXeEfgGaAcspn]')
EXT = ('.cpp', '.c', '.cc', '.h', '.hpp', '.inc')


def literals(src):
    """yield (line, text) for every string literal, with None as a break marker when anything but whitespace/comments separates two literals."""
    i, n, line = 0, len(src), 1
    last_end_was_literal = False
    while i < n:
        c = src[i]
        if c == '\n':
            line += 1; i += 1
        elif c in ' \t\r\\':
            i += 1
        elif src.startswith('//', i):
            j = src.find('\n', i)
            i = n if j < 0 else j
        elif src.startswith('/*', i):
            j = src.find('*/', i + 2)
            j = n if j < 0 else j + 2
            line += src.count('\n', i, j); i = j
        elif c == '"':
            j = i + 1
            while j < n and src[j] != '"' and src[j] != '\n':
                j += 2 if src[j] == '\\' else 1
            yield line, src[i + 1:j]
            last_end_was_literal = True
            i = j + 1
            continue
        elif c == "'":
            j = i + 1
            while j < n and src[j] != "'" and src[j] != '\n':
                j += 2 if src[j] == '\\' else 1
            i = j + 1
            yield None, None
        else:
            i += 1
            if not c.isspace():
                yield None, None


def check(path):
    try:
        src = open(path, encoding='utf-8', errors='replace').read()
    except OSError:
        return []
    hits, run, first = [], [], 0
    def flush():
        if len(run) > 1:
            joined = ''.join(run)
            if BAD.search(joined) and not any(BAD.search(r) for r in run):
                hits.append((first, joined))
        run.clear()
    for line, text in literals(src):
        if text is None:
            flush()
        else:
            if not run:
                first = line
            run.append(text)
    flush()
    return hits


def main():
    root, dirs = sys.argv[1], sys.argv[2:]
    found = 0
    for d in dirs:
        for base, _, files in os.walk(os.path.join(root, d)):
            for f in sorted(files):
                if f.endswith(EXT):
                    p = os.path.join(base, f)
                    for line, joined in check(p):
                        print(f'{os.path.relpath(p, root)}:{line}: %z/%t/%j/%hh in a format string split across adjacent literals: "{joined[:100]}"')
                        found += 1
    return 1 if found else 0


if __name__ == '__main__':
    sys.exit(main())
