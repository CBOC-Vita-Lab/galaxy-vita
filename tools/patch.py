#!/usr/bin/env python3
"""Tiny helper for applying exact, verified text substitutions to decomp files.

Usage from other scripts:
    from patch import sub
    sub('path', 'old', 'new')           # exactly one occurrence required
    sub('path', 'old', 'new', count=0)  # replace all, at least one required
"""
import os
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'decomp')
_failures = []


def read(path):
    with open(os.path.join(ROOT, path), 'rb') as fh:
        return fh.read().decode('utf-8', errors='surrogateescape')


def write(path, text):
    with open(os.path.join(ROOT, path), 'wb') as fh:
        fh.write(text.encode('utf-8', errors='surrogateescape'))


def sub(path, old, new, count=1):
    text = read(path)
    n = text.count(old)
    if n == 0:
        if new in text:
            return  # already applied
        _failures.append('%s: pattern not found: %r' % (path, old[:80]))
        return
    if count == 1 and n != 1:
        _failures.append('%s: pattern found %d times (expected 1): %r' % (path, n, old[:80]))
        return
    text = text.replace(old, new) if count == 0 else text.replace(old, new, count)
    write(path, text)


def done():
    if _failures:
        print('\n'.join(_failures))
        sys.exit(1)
    print('ok')
