#!/usr/bin/env python3
"""Inventory the file types contained in every archive on the extracted disc."""
import os
import sys
import collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from rarc import yaz0_decompress, rarc_list

root = sys.argv[1]
ext_count = collections.Counter()
ext_bytes = collections.Counter()
magic_by_ext = collections.defaultdict(collections.Counter)
examples = {}
nested_yaz0 = 0
for dp, _, files in os.walk(root):
    for f in files:
        if not f.endswith(('.arc', '.szs')):
            continue
        p = os.path.join(dp, f)
        raw = open(p, 'rb').read()
        try:
            data = yaz0_decompress(raw)
            for path, blob in rarc_list(data):
                ext = os.path.splitext(path)[1].lower() or '(none)'
                ext_count[ext] += 1
                ext_bytes[ext] += len(blob)
                if blob[:4] == b'Yaz0':
                    nested_yaz0 += 1
                magic_by_ext[ext][blob[:4]] += 1
                examples.setdefault(ext, os.path.relpath(p, root) + ':' + path)
        except Exception as e:
            print('ERR', p, e)

for ext, n in ext_count.most_common():
    mags = ', '.join('%r:%d' % (m, c) for m, c in magic_by_ext[ext].most_common(3))
    print('%-8s %6d files %9d KB  magic %s  e.g. %s' % (ext, n, ext_bytes[ext] // 1024, mags, examples[ext]))
print('nested Yaz0 blobs:', nested_yaz0)
