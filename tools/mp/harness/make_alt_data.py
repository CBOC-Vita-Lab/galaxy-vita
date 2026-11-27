#!/usr/bin/env python3
"""A "different version" of the cooked disc for the version-mismatch test, without copying the game data.

  make_alt_data.py <cooked-disc> <out-dir> [stage-file-substring]

<out-dir>/files is a symlink to <cooked-disc>/files; <out-dir>/sys holds symlinks to every sys file except fst.bin, which is a
copy with the size field of one StageData file (default: the first whose path contains "koopabattlevs3", a galaxy the tests
never load) increased by 4. The multiplayer fingerprint (platform/src/mp/mp_boot.cpp: CRC of the StageData file list with
sizes) then differs, as between two releases, while every file the test loads reads as before. Local test use only.
"""
import os, struct, sys


def main():
    src, out = sys.argv[1], sys.argv[2]
    want = (sys.argv[3] if len(sys.argv) > 3 else 'koopabattlevs3').lower()
    os.makedirs(os.path.join(out, 'sys'), exist_ok=True)
    if not os.path.lexists(os.path.join(out, 'files')):
        os.symlink(os.path.join(src, 'files'), os.path.join(out, 'files'))
    for n in os.listdir(os.path.join(src, 'sys')):
        if n != 'fst.bin' and not os.path.lexists(os.path.join(out, 'sys', n)):
            os.symlink(os.path.join(src, 'sys', n), os.path.join(out, 'sys', n))
    fst = bytearray(open(os.path.join(src, 'sys', 'fst.bin'), 'rb').read())
    count = struct.unpack_from('>I', fst, 8)[0]
    names = count * 12
    stack = [(count, '')]
    for i in range(1, count):
        while len(stack) > 1 and i >= stack[-1][0]:
            stack.pop()
        e = i * 12
        off = struct.unpack_from('>I', fst, e)[0] & 0xFFFFFF
        name = fst[names + off:fst.index(0, names + off)].decode('latin-1').lower()
        full = stack[-1][1] + name
        if fst[e]:
            stack.append((struct.unpack_from('>I', fst, e + 8)[0], full + '/'))
            continue
        if full.startswith('stagedata/') and want in full:
            size = struct.unpack_from('>I', fst, e + 8)[0]
            struct.pack_into('>I', fst, e + 8, size + 4)
            open(os.path.join(out, 'sys', 'fst.bin'), 'wb').write(fst)
            print(f'make_alt_data: {full} size {size} -> {size + 4} in {out}/sys/fst.bin')
            return 0
    print(f'make_alt_data: no StageData file matching {want!r}', file=sys.stderr)
    return 1


if __name__ == '__main__':
    sys.exit(main())
