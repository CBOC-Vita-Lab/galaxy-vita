"""Count compact-mip candidates in cooked ObjectData, not draws or GPU timings."""
import argparse
import collections
import hashlib
from pathlib import Path
import struct

from farlod_extract import blocks, models


def census(root):
    seen = set()
    totals = collections.Counter()
    by_format = collections.Counter()
    for arc in root.rglob("*.arc"):
        for model in models(arc.read_bytes()):
            tex = blocks(model).get(b"TEX1")
            if not tex:
                continue
            start, nbytes = tex
            b = model[start:start + nbytes]
            count, = struct.unpack_from("<H", b, 8)
            header, = struct.unpack_from("<I", b, 12)
            for i in range(count):
                at = header + 32 * i
                fmt, _, w, h = struct.unpack_from("<BBHH", b, at)
                if not (fmt in (1, 2, 3) and w == h and 8 <= w <= 1024 and not w & (w - 1)
                        and b[at + 16] and b[at + 20] in (2, 3, 4, 5) and b[at + 24] > 1):
                    continue
                size = w*h*(2 if fmt == 3 else 1)
                offset = at + struct.unpack_from("<I", b, at + 28)[0]
                if offset < at + 32 or offset + size > len(b):
                    continue
                key = (fmt, w, hashlib.sha256(b[offset:offset+size]).digest())
                totals["records"] += 1
                if key in seen:
                    continue
                seen.add(key)
                by_format[fmt] += 1
                totals["base_bytes_saved"] += w*h*2
                # Swizzled Vita chain stops at 2x2. Includes level zero.
                totals["swizzled_bytes_saved"] += sum(2*(w >> j)**2 for j in range(w.bit_length()-1))
    return len(seen), dict(sorted(by_format.items())), dict(totals)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("object_data", type=Path)
    root = parser.parse_args().object_data
    if not root.is_dir():
        parser.error("missing ObjectData directory")
    print("Unique payloads (I8=1, IA4=2, IA8=3), static byte potential if used once; excludes runtime overrides.")
    print(census(root))
