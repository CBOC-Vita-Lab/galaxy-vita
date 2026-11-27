"""Read authored skinning topology from the user's cooked models; no game data is distributed."""
import argparse
from pathlib import Path
import struct
from farlod_extract import blocks, models


def topology(model):
    entry = blocks(model).get(b'EVP1')
    if not entry:
        return None
    start, size = entry
    b = memoryview(model)[start:start+size]
    n, mix, idx = struct.unpack_from('<H2xII', b, 8)
    if not n:
        return None
    counts = list(b[mix:mix+n])
    return counts, struct.unpack_from(f'<{sum(counts)}H', b, idx)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('root', type=Path)
    p.add_argument('--fixture', type=Path, help='temporary benchmark input, selected models only')
    p.add_argument('--all', action='store_true', help='benchmark every eligible model')
    a = p.parse_args()
    names = {'Mario', 'Luigi', 'Penguin', 'Tico', 'HoneyBee', 'HoneyQueen', 'Kinopio', 'Koopa'}
    total = products = misses = 0
    selected = []
    for path in sorted(a.root.rglob('*.arc')):
        for b in models(path.read_bytes()):
            top = topology(b)
            if top is None:
                continue
            counts, indices = top
            if len(counts) >= 4:
                keys = [None] * 16
                new = 0
                for j in indices:
                    if keys[j % 16] != j:
                        keys[j % 16] = j
                        new += 1
                total += 1
                products += len(indices)
                misses += new
            if path.stem in names or len(counts) >= 220 or (a.all and len(counts) >= 4):
                selected.append(' '.join(map(str, (path.stem, len(counts), *counts, *indices))))
    print(f'EVP1 (>=4 envelopes): {total} cooked models, {products} joint products -> {misses} (one pose each, not runtime frequency)')
    if a.fixture:
        a.fixture.write_text('\n'.join(selected) + '\n')


if __name__ == '__main__':
    main()
