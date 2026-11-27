#!/usr/bin/env python3
# Galaxy 3.0 multiplayer: ground-truth check of the world registry keys against the cooked stage data (the owner's disc, never shipped).
# For every zone archive: every placed row (placement / mapparts / childobj tables) gets the key MpWorld builds,
# (15-bit FNV name hash of the placement name as in mp_name_hash15, l_id & 0xFFFF). Fails when two rows of one zone share a key
# (the registry would mark both as duplicates: never synced) or when a zone has more rows than the registry holds.
# Usage: python3 -I tests/mp/world_keys_data.py <dir with rarc.py> <StageData dir> [r-world survey.py]
import collections, importlib.util, os, sys

tools, stage = sys.argv[1], sys.argv[2]
survey = sys.argv[3] if len(sys.argv) > 3 else '/home/claude/g3/research/r-world/tools/survey.py'
argv = sys.argv
sys.argv = [survey, tools, stage]
spec = importlib.util.spec_from_file_location('sv', survey)
sv = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sv)
sys.argv = argv
sv.H[sv.jhash('type')] = 'type'   # MR::getObjectName reads "type" first, then "name"


def hash15(name_bytes):
    h = 2166136261
    for c in name_bytes:
        h ^= c
        h = (h * 16777619) & 0xFFFFFFFF
    return (h ^ (h >> 15)) & 0x7FFF


zones = sorted(f[:-4] for f in os.listdir(stage) if f.endswith('.arc'))
bad = 0
busiest = (0, '')
checked = 0
for z in zones:
    files = sv.zone(z)
    if not files:
        continue
    keys = collections.Counter()
    names = {}
    for path, rows in files:
        for r in rows:
            n = r.get('type') or r.get('name')
            if not isinstance(n, str):
                continue
            raw = n.encode('shift_jis', 'replace')
            lid = r.get('l_id')
            lid = 0xFFFF if lid is None else (lid & 0xFFFF)
            k = (hash15(raw), lid)
            keys[k] += 1
            names.setdefault(k, set()).add(n)
    checked += 1
    total = sum(keys.values())
    busiest = max(busiest, (total, z))
    dups = {k: c for k, c in keys.items() if c > 1}
    if dups:
        bad += 1
        print(f'{z}: {len(dups)} duplicate keys, e.g.', [(hex(k[0]), k[1], sorted(names[k])) for k in list(dups)[:3]])
    if total > 2560:
        bad += 1
        print(f'{z}: {total} rows > registry capacity')
print(f'world_keys_data: zones={checked} busiest={busiest[1]} rows={busiest[0]} bad={bad}')
sys.exit(1 if bad else 0)
