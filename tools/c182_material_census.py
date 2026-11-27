"""Static histogram of inverse-eligible MAT3 texture slots in cooked ObjectData (not a draw census)."""
import collections
import pathlib
import struct
import sys

from farlod_extract import blocks, models


def material_modes(model):
    mat = blocks(model).get(b'MAT3')
    if not mat:
        return
    start, length = mat
    b = memoryview(model)[start:start + length]
    u16 = lambda off: struct.unpack_from('<H', b, off)[0]
    init, remap, _, _, _, _, _, _, _, _, ngen, coords, _, matrices = struct.unpack_from('<14I', b, 12)
    for index in range(u16(8)):
        record = init + u16(remap + 2 * index) * 0x14c
        modes = []
        for i in range(min(8, b[ngen + b[record + 3]])):
            coord = u16(record + 0x28 + 2 * i)
            mtx = u16(record + 0x48 + 2 * i)
            if coords and matrices and coord != 0xffff and mtx != 0xffff and b[coords + 4 * coord] < 2:
                modes.append(b[matrices + 0x64 * mtx + 1] & 63)
        yield modes


def main(root):
    hist = [collections.Counter(), collections.Counter()]
    examples = []
    arc_count = model_count = 0
    for path in sorted(root.rglob('*.arc')):
        arc_count += 1
        for model in models(path.read_bytes()):
            model_count += 1
            for modes in material_modes(model):
                for bucket, eligible in zip(hist, ({5, 10, 11}, {2, 5, 8, 10, 11})):
                    n = sum(mode in eligible for mode in modes)
                    bucket[n] += 1
                    if n >= 2 and len(examples) < 15 and path.stem not in examples:
                        examples.append(path.stem)
    print(f'{arc_count} archives; {model_count} models; authored material records per # eligible slots (not runtime frequency):')
    print('regular', sorted(hist[0].items()), 'post', sorted(hist[1].items()))
    print('first multi-eligible archives:', ', '.join(examples) or 'none')


if __name__ == '__main__':
    main(pathlib.Path(sys.argv[1]))
