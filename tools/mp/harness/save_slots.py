#!/usr/bin/env python3
# savefix: per-slot checks of a process's single-player GameData.bin for scenario rules (assert_log.py verbs):
#   save_valid P      P's single-player GameData.bin is a whole, valid file (version, table, length, checksum) after the run
#   save_mine P N     "Play on my file N": the file after the run differs from the newest backup in <run>/mp/backups only in file N's
#                     blocks (plus sysconf's save-time stamp, which every game save rewrites), and that backup is the file as it was when the run started (save-before.json, or the run it continued)
import hashlib, json, os, struct

DATA = 'title/00010000/524d4750/data/GameData.bin'


def parse(b):
    """(ok, why, {name: (offset, size)}) for a GameData.bin image (mp_save_validate: version 2, count < 24, size, length, checksum)."""
    if len(b) < 16:
        return False, 'short', {}
    _, version, count, size = struct.unpack_from('<IIII', b, 0)
    if version != 2:
        return False, 'version %d' % version, {}
    if count == 0 or count >= 24:
        return False, 'count', {}
    if size >= 0x10000 or size < 16 + count * 16 or len(b) < size:
        return False, 'size', {}
    if len(b) != (size + 31) & ~31:
        return False, 'length', {}
    ents = []
    for i in range(count):
        name = b[16 + i * 16:16 + i * 16 + 12].split(b'\0')[0].decode('ascii', 'replace')
        off = struct.unpack_from('<I', b, 16 + i * 16 + 12)[0]
        ents.append((name, off))
    blocks = {}
    for i, (name, off) in enumerate(ents):
        end = ents[i + 1][1] if i + 1 < len(ents) else size
        if off > end or end > size:
            return False, 'table', {}
        blocks[name] = (off, end - off)
    s = inv = 0
    for i in range(4, size - 1, 2):
        w = b[i] | (b[i + 1] << 8)
        s = (s + w) & 0xFFFF
        inv = (inv + (~w & 0xFFFF)) & 0xFFFF
    if struct.unpack_from('<I', b, 0)[0] != ((s << 16) | inv):
        return False, 'checksum', {}
    return True, 'ok', blocks


def _read(p):
    try:
        with open(p, 'rb') as f:
            return f.read()
    except OSError:
        return None


def check(proc, verb, words):
    sp = proc.file(os.path.join('saves', DATA))
    after = _read(sp)
    if after is None:
        return False, 'no single-player GameData.bin after the run'
    ok, why, blocks = parse(after)
    if not ok:
        return False, 'single-player GameData.bin invalid after the run (%s)' % why
    if verb == 'save_valid':
        return True, 'single-player GameData.bin valid (%s)' % hashlib.sha256(after).hexdigest()[:16]
    n = int(words[0]) if words else 0
    bdir = proc.file(os.path.join('mp', 'backups'))
    names = sorted(x for x in os.listdir(bdir) if x.startswith('GameData-') and x.endswith('.bin')) if os.path.isdir(bdir) else []
    if not names:
        return False, 'no backup in mp/backups'
    # the session's backup: the newest one of this run's start (by sequence: names sort by it)
    before = proc.json('save-before.json') or {}
    want = None
    for k, v in (before.get('files') or {}).items():
        if k.endswith('GameData.bin'):
            want = v.get('sha256') if isinstance(v, dict) else v
    bk = None
    for name in reversed(names):
        data = _read(os.path.join(bdir, name))
        if data is not None and (want is None or hashlib.sha256(data).hexdigest() == want):
            bk = (name, data)
            break
    if bk is None:
        return False, 'no backup holds the file as it was at the start (%d backups)' % len(names)
    okb, whyb, bblocks = parse(bk[1])
    if not okb:
        return False, 'backup %s invalid (%s)' % (bk[0], whyb)
    changed = sorted(name for name in blocks if name in bblocks and after[blocks[name][0]:sum(blocks[name])] != bk[1][bblocks[name][0]:sum(bblocks[name])])
    allowed = {'mario%d' % n, 'luigi%d' % n, 'config%d' % n, 'sysconf'}   # sysconf: the game's own save stamps its save time in it (any single-player save does)
    other = [c for c in changed if c not in allowed]
    if other:
        return False, 'blocks outside file %d changed: %s (backup %s)' % (n, ', '.join(other), bk[0])
    return True, 'only file %d changed (%s); backup %s holds the start' % (n, ', '.join(changed) or 'nothing', bk[0])
