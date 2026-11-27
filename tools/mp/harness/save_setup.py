#!/usr/bin/env python3
"""HARNESS_SAVE_SETUP: pre-seeds the save folder of ONE run (run_one.sh calls it after the copy of the test save / the continued saves, before save-before.json).

  save_setup.py <run>/saves "op,op,..."

It edits ONLY the folder that holds the single-player GameData.bin inside the run's own copied save tree (<saves>/title/00010000/524d4750/data), never anything
else; the owner's real saves are never reachable from here (run_one.sh copies the test save into <run>/saves first). Ops run in order, each on the state the
previous one left:
  tear    GameData.bin keeps its first half; the second half is overwritten with 0xA5 bytes (same size; the checksum / table no longer fit: it fails validation)
  mpbak   copy GameData.bin (as it is now) to GameData.bin.mpbak
  mptmp   copy GameData.bin (as it is now) to GameData.bin.mptmp
  rm      delete GameData.bin only (a .mpbak / .mptmp stay)
Examples: "mptmp,tear" = torn file + a good .mptmp;  "mpbak,tear" = torn file + a good .mpbak;  "mpbak,rm" = a deleted save with its .mpbak left.
Exit 0 ok, 2 bad op / missing file / folder outside the run.
"""
import os, shutil, sys

DATA = os.path.join('title', '00010000', '524d4750', 'data')


def apply(saves, spec):
    saves = os.path.realpath(saves)
    d = os.path.realpath(os.path.join(saves, DATA))
    if os.path.commonpath([saves, d]) != saves:
        raise SystemExit('save_setup: data folder is outside the run saves')
    f = os.path.join(d, 'GameData.bin')
    for n in ('', '.mpbak', '.mptmp'):   # a symlink in the folder could lead an op outside the run: refuse
        if os.path.islink(f + n):
            raise SystemExit(f'save_setup: GameData.bin{n} is a symlink: refused')
    for op in [o.strip() for o in spec.split(',') if o.strip()]:
        if op == 'tear':
            b = bytearray(open(f, 'rb').read())
            if len(b) < 32:
                raise SystemExit('save_setup: tear: GameData.bin too short')
            h = len(b) // 2
            b[h:] = b'\xa5' * (len(b) - h)
            open(f, 'wb').write(bytes(b))
        elif op in ('mpbak', 'mptmp'):
            shutil.copyfile(f, f + '.' + op)
        elif op == 'rm':
            os.unlink(f)
        else:
            raise SystemExit(f'save_setup: unknown op {op!r} (tear, mpbak, mptmp, rm)')
    return 0


if __name__ == '__main__':
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    try:
        apply(sys.argv[1], sys.argv[2])
    except SystemExit as e:
        print(e.code if not isinstance(e.code, int) else '', file=sys.stderr)
        sys.exit(2)
    except OSError as e:
        print(f'save_setup: {e}', file=sys.stderr)
        sys.exit(2)
