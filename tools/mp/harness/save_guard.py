#!/usr/bin/env python3
"""Single-player save guard for the MP test harness.

  save_guard.py hash <saves-dir> <out.json>          sha256 of every GameData.bin (and banner.bin) under <saves-dir>
  save_guard.py compare <before.json> <after.json> <out.json>
                                                     verdict: the single-player GameData.bin byte-identical or not;
                                                     every other save file listed as added / changed / removed
The hash also records the save folder's file list (name and size of EVERY file beside GameData.bin: .mpbak, .mptmp, .mpbad*, ...); compare
reports any change of that list as a separate finding (folder_changed, folder_added / folder_removed / folder_resized) and prints a
"FINDING" line; it does not change the exit status.
Exit status of compare: 0 unchanged, 1 the single-player file changed or vanished, 2 usage.
The single-player file is title/00010000/524d4750/data/GameData.bin (the Wii NAND layout the port keeps on every platform).
"""
import hashlib, json, os, sys

SP = 'title/00010000/524d4750/data/GameData.bin'


def digest(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 16), b''):
            h.update(block)
    return h.hexdigest()


def hash_tree(root):
    out = {}
    if not os.path.isdir(root):
        return out
    for d, _, files in os.walk(root):
        for n in files:
            if n in ('GameData.bin', 'banner.bin') or n.endswith('.bin'):
                p = os.path.join(d, n)
                out[os.path.relpath(p, root)] = {'sha256': digest(p), 'bytes': os.path.getsize(p)}
    return dict(sorted(out.items()))


def folder_list(root):
    """name -> size of every file in the folder that holds the single-player GameData.bin (not recursive)."""
    d = os.path.join(root, os.path.dirname(SP))
    out = {}
    if os.path.isdir(d):
        for n in os.listdir(d):
            p = os.path.join(d, n)
            if os.path.isfile(p):
                out[n] = os.path.getsize(p)
    return dict(sorted(out.items()))


def main(argv):
    if len(argv) == 4 and argv[1] == 'hash':
        res = {'root': os.path.abspath(argv[2]), 'files': hash_tree(argv[2]), 'folder': folder_list(argv[2])}
        with open(argv[3], 'w') as f:
            json.dump(res, f, indent=1)
        sp = res['files'].get(SP)
        print(f"save guard: {len(res['files'])} file(s); single-player GameData.bin {sp['sha256'][:16] if sp else 'MISSING'}")
        return 0
    if len(argv) == 5 and argv[1] == 'compare':
        a = json.load(open(argv[2]))['files']
        b = json.load(open(argv[3]))['files']
        sp_a, sp_b = a.get(SP), b.get(SP)
        res = {
            'single_player_file': SP,
            'before': sp_a['sha256'] if sp_a else None,
            'after': sp_b['sha256'] if sp_b else None,
            'unchanged': bool(sp_a and sp_b and sp_a['sha256'] == sp_b['sha256']),
            'added': sorted(set(b) - set(a)),
            'removed': sorted(set(a) - set(b)),
            'changed': sorted(k for k in set(a) & set(b) if a[k]['sha256'] != b[k]['sha256']),
        }
        fa, fb = json.load(open(argv[2])).get('folder'), json.load(open(argv[3])).get('folder')
        if fa is not None and fb is not None:
            res['folder_added'] = sorted(set(fb) - set(fa))
            res['folder_removed'] = sorted(set(fa) - set(fb))
            res['folder_resized'] = {k: [fa[k], fb[k]] for k in sorted(set(fa) & set(fb)) if fa[k] != fb[k]}
            res['folder_changed'] = bool(res['folder_added'] or res['folder_removed'] or res['folder_resized'])
        else:
            res['folder_changed'] = None   # (a record from an older guard: no file list to compare)
        with open(argv[4], 'w') as f:
            json.dump(res, f, indent=1)
        state = 'UNCHANGED' if res['unchanged'] else ('MISSING' if not sp_a or not sp_b else 'CHANGED')
        extra = ''.join(f' {k}={len(res[k])}' for k in ('added', 'removed', 'changed') if res[k])
        print(f'save guard: single-player GameData.bin {state}{extra}')
        if res['folder_changed']:
            print(f"save guard: FINDING save folder file list changed: added={res['folder_added']} removed={res['folder_removed']} "
                  f"resized={res['folder_resized']}")
        return 0 if res['unchanged'] else 1
    sys.stderr.write(__doc__)
    return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv))
