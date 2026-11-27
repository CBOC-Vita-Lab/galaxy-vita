#!/usr/bin/env python3
"""Galaxy 3.0 multiplayer (fix2): checks the real scenario data behind MpFlow's scenario range (no game code).

For every StageData/<galaxy>/<galaxy>Scenario.arc: ScenarioNo in ScenarioData.bcsv runs exactly 1..rows (so the row count is the
highest scenario a MOVE / WHERE may name, hidden and green stars included), and the galaxies with hidden scenarios are exactly
tests/mp/flow_test.cpp's kHiddenScenarios table (rows, visible).

    python3 -I tests/mp/scenario_data_check.py [/data/cooked]     (skips with exit 0 when the data is not there)
The cooked archives are the port's little-endian RARC ("CRAR"), Yaz0 or plain; big-endian (disc) archives work too.
"""
import glob
import os
import re
import struct
import sys


def yaz0(d):
    if d[:4] != b'Yaz0':
        return d
    size = struct.unpack('>I', d[4:8])[0]
    out = bytearray()
    i = 16
    while len(out) < size:
        code = d[i]
        i += 1
        for b in range(8):
            if len(out) >= size:
                break
            if code & (0x80 >> b):
                out.append(d[i])
                i += 1
            else:
                b1, b2 = d[i], d[i + 1]
                i += 2
                dist = ((b1 & 0xF) << 8 | b2) + 1
                n = b1 >> 4
                if n == 0:
                    n = d[i] + 0x12
                    i += 1
                else:
                    n += 2
                for _ in range(n):
                    out.append(out[-dist])
    return bytes(out)


def rarc_files(d):
    e = '<' if d[:4] == b'CRAR' else '>'
    dataoff = struct.unpack(e + 'I', d[12:16])[0] + 0x20
    _nn, _noff, ne, eoff, _ssz, soff = struct.unpack(e + 'IIIIII', d[0x20:0x38])
    eoff += 0x20
    soff += 0x20
    files = {}
    for k in range(ne):
        _idx, _h, w, doff, sz, _p = struct.unpack(e + 'HHIIII', d[eoff + k * 20: eoff + k * 20 + 20])
        typ, noff = w >> 24, w & 0xFFFF
        if typ & 0x2:
            continue
        name = d[soff + noff: d.index(b'\0', soff + noff)].decode()
        files[name.lower()] = d[dataoff + doff: dataoff + doff + sz]
    return files, e


def jhash(s):
    h = 0
    for c in s.encode('shift_jis'):
        if c >= 0x80:
            c -= 256
        h = (h * 31 + c) & 0xFFFFFFFF
    return h


def bcsv_column(d, e, name):
    ne, nf, doff, esz = struct.unpack(e + 'IIII', d[:16])
    want = jhash(name)
    for k in range(nf):
        h, mask, w = struct.unpack(e + 'III', d[16 + k * 12: 28 + k * 12])
        if e == '<':
            off, shift, typ = w & 0xFFFF, (w >> 16) & 0xFF, w >> 24
        else:
            off, shift, typ = w >> 16, (w >> 8) & 0xFF, w & 0xFF
        if h != want:
            continue
        vals = []
        for r in range(ne):
            base = doff + r * esz
            if typ in (0, 3):
                v = struct.unpack(e + 'I', d[base + off: base + off + 4])[0]
            elif typ == 4:
                v = struct.unpack(e + 'H', d[base + off: base + off + 2])[0]
            elif typ == 5:
                v = d[base + off]
            else:
                raise ValueError('column %s: type %d' % (name, typ))
            vals.append((v & mask) >> shift)
        return vals
    return [0] * ne


def name_hash15(s):
    # mp_name_hash15 (platform/include/port/mp_proto.h): FNV-1a over the bytes, folded to 15 bits. STAR / STARV name a galaxy by it (starfix).
    h = 2166136261
    for b in s.encode('ascii'):
        h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    return (h ^ (h >> 15)) & 0x7FFF


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else '/data/cooked'
    arcs = sorted(glob.glob(os.path.join(root, 'files', 'StageData', '*', '*Scenario.arc')))
    if not arcs:
        print('scenario data check: no data under %s, skipped' % root)
        return 77   # (exit 77 = SKIPPED, the convention of keepsafe_build.sh: allsuites lists it as SKIP, not PASS)
    here = os.path.dirname(os.path.abspath(__file__))
    src = open(os.path.join(here, 'flow_test.cpp'), encoding='utf-8').read()
    body = src[src.index('scenario-data-table-begin'): src.index('scenario-data-table-end')]
    table = {g: (int(r), int(v)) for g, r, v in re.findall(r'\{"(\w+)", (\d+), (\d+)\}', body)}
    fails = 0
    found = {}
    for path in arcs:
        galaxy = os.path.basename(path)[:-len('Scenario.arc')]
        files, e = rarc_files(yaz0(open(path, 'rb').read()))
        d = files['scenariodata.bcsv']
        nos = bcsv_column(d, e, 'ScenarioNo')
        hidden = bcsv_column(d, e, 'IsHidden')
        if sorted(nos) != list(range(1, len(nos) + 1)):
            print('FAIL %s: ScenarioNo %s is not 1..%d' % (galaxy, sorted(nos), len(nos)))
            fails += 1
        h = sum(1 for x in hidden if x)
        if h:
            found[galaxy] = (len(nos), len(nos) - h)
    seen = {}
    for path in arcs:   # no two galaxy names share a hash: a STARV's galaxy is found by it on every console
        galaxy = os.path.basename(path)[:-len('Scenario.arc')]
        hv = name_hash15(galaxy)
        if hv in seen:
            print('FAIL galaxy name hash collision: %s and %s (%04x)' % (seen[hv], galaxy, hv))
            fails += 1
        seen[hv] = galaxy
    if found != table:
        print('FAIL hidden-scenario galaxies differ from flow_test.cpp:\n  data  %s\n  table %s' % (sorted(found.items()), sorted(table.items())))
        fails += 1
    print('scenario data check: %d galaxies, %d with hidden scenarios, max rows %d, %d failures' %
          (len(arcs), len(found), max(r for r, _ in found.values()) if found else 0, fails))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
