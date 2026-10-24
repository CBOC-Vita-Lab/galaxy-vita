#!/usr/bin/env python3
"""Re-encode non-ASCII characters inside C/C++ string and char literals as
Shift-JIS (cp932) byte escapes.

The original game was compiled by CodeWarrior with `-enc SJIS`, so every
narrow string literal in the binary is Shift-JIS.  Level data (BCSV tables,
camera names, object names) is compared against those literals at runtime, so
the port must produce identical bytes.  Clang only understands UTF-8 source, so
we rewrite  "カメラ"  as  "\\x83\\x4a\\x83\\x81\\x83\\x89"  in place.

Comments are left untouched (UTF-8 is fine there).  Wide literals (L"...")
are left untouched too: CodeWarrior converted those to UTF-16 as well.

Usage: sjis_literals.py <root> [<root> ...]
"""
import sys
import os

EXTS = ('.c', '.cpp', '.h', '.hpp', '.inc', '.cp')


def is_hex(ch):
    return ch in '0123456789abcdefABCDEF'


def convert_literal_body(body):
    """body is the text between the quotes (escapes intact). Returns new body."""
    out = []
    i = 0
    n = len(body)
    last_was_hex_escape = False
    while i < n:
        ch = body[i]
        if ch == '\\' and i + 1 < n:
            # copy escape sequence verbatim
            j = i + 1
            if body[j] == 'x':
                j += 1
                while j < n and is_hex(body[j]):
                    j += 1
                esc = body[i:j]
                if last_was_hex_escape is None:
                    pass
                out.append(esc)
                last_was_hex_escape = True
                i = j
                continue
            elif body[j] in '01234567':
                k = j
                while k < n and k < j + 3 and body[k] in '01234567':
                    k += 1
                out.append(body[i:k])
                last_was_hex_escape = False
                i = k
                continue
            else:
                out.append(body[i:j + 1])
                last_was_hex_escape = False
                i = j + 1
                continue
        if ord(ch) < 0x80:
            if last_was_hex_escape and is_hex(ch):
                # terminate the previous \x escape so this digit is not absorbed
                out.append('""')
            out.append(ch)
            last_was_hex_escape = False
            i += 1
            continue
        # non-ASCII: encode as cp932 bytes
        try:
            data = ch.encode('cp932')
        except UnicodeEncodeError:
            raise
        for b in data:
            out.append('\\x%02x' % b)
        last_was_hex_escape = True
        i += 1
    return ''.join(out)


def process(text):
    out = []
    i = 0
    n = len(text)
    changed = False
    while i < n:
        ch = text[i]
        # line comment
        if ch == '/' and i + 1 < n and text[i + 1] == '/':
            j = text.find('\n', i)
            if j < 0:
                j = n
            out.append(text[i:j])
            i = j
            continue
        # block comment
        if ch == '/' and i + 1 < n and text[i + 1] == '*':
            j = text.find('*/', i + 2)
            j = n if j < 0 else j + 2
            out.append(text[i:j])
            i = j
            continue
        if ch == '"' or ch == "'":
            quote = ch
            # detect prefix (L, u8, u, U) immediately before the quote
            prefix = ''
            k = len(out) - 1
            # look back in already-emitted text for an identifier char prefix
            back = ''.join(out[-1:]) if out else ''
            wide = back.endswith('L') or back.endswith('u') or back.endswith('U')
            j = i + 1
            while j < n:
                c = text[j]
                if c == '\\':
                    j += 2
                    continue
                if c == quote or c == '\n':
                    break
                j += 1
            body = text[i + 1:j]
            if not wide and any(ord(c) >= 0x80 for c in body):
                newbody = convert_literal_body(body)
                if newbody != body:
                    changed = True
                out.append(quote + newbody + quote)
            else:
                out.append(quote + body + quote)
            i = j + 1
            continue
        # copy run of ordinary chars quickly
        j = i + 1
        while j < n and text[j] not in '/"\'':
            j += 1
        out.append(text[i:j])
        i = j
    return ''.join(out), changed


def main():
    total = 0
    for root in sys.argv[1:]:
        for dirpath, _, files in os.walk(root):
            for f in files:
                if not f.endswith(EXTS):
                    continue
                p = os.path.join(dirpath, f)
                with open(p, 'rb') as fh:
                    raw = fh.read()
                try:
                    text = raw.decode('utf-8')
                except UnicodeDecodeError:
                    print('skip (not utf-8):', p)
                    continue
                if not any(ord(c) >= 0x80 for c in text):
                    continue
                new, changed = process(text)
                if changed:
                    with open(p, 'wb') as fh:
                        fh.write(new.encode('utf-8'))
                    total += 1
    print('rewrote', total, 'files')


if __name__ == '__main__':
    main()
