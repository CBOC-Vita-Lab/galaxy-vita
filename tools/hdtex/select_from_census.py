#!/usr/bin/env python3
"""Turns the console's texture magnification census into an HD texture selection (stdlib + numpy only).

    python3 tools/hdtex/select_from_census.py texmag-*.txt --list list-all.tsv \
        [--total-mb 48] [--stage-mb 20] [--min-area 2000] [--include-player] \
        [--out selection-census.txt] [--report census-report.txt]

  list-all.tsv   build_pack.py --data <cooked/files> --list-all > list-all.tsv   (every eligible texture: key, fmt, w, h, alpha,
                 the bytes its 2x BC1/BC3 mip chain takes on the GPU, archive:texture names)
  texmag-*.txt   one file per stage (GXV_TEXMAG_CENSUS=N on the console, ux0:data/smg/): pixel columns are display pixels summed over
                 the sampled pictures; this tool divides by pictures_sampled to get pixels per picture.

The rule (the maintainer's): a texture gets a 2x replacement only where it is magnified (>= 1.5 display pixels per texel, linear) over at
least A pixels of a picture (--min-area, default 2000) in at least one stage; 4x never. Candidates are ranked by sharpened pixels per GPU byte
and taken while the pack stays within --total-mb and no stage's resident HD bytes pass --stage-mb (a texture that would overflow any stage it
is seen in is left out, and the report says so). --include-player adds the player's textures (Mario, Luigi: the census flags them from the
pack's list) whatever their magnification when they cover at least A pixels: they are on screen all the time and the owner looks at them.

Writes the selection file build_pack.py accepts (--select-keys: one 16-digit key per line, the rest of the line is a comment) and a per-stage
report: GPU bytes added, pixels sharpened.  Census textures missing from the list (not an eligible format, too small, or in no archive the
list scanned) are counted and reported, never selected.
"""
import argparse
import glob
import os
import sys

# Rough bytes per texel of what the cache holds for an original of each GX format (with mips: x 4/3); only for the "added" column.
ORIG_BPP = {'CMPR': 0.5, 'RGB565': 2.0, 'RGB5A3': 2.0, 'RGBA8': 4.0, 'C4': 2.0, 'C8': 2.0, 'C14X2': 2.0}
MIPS = 4.0 / 3.0


def read_list(path):
    """{key: dict(fmt, w, h, alpha, bytes, names)} from build_pack.py --list-all (TSV; '#' lines skipped) or the older --list text."""
    out = {}
    for line in open(path):
        line = line.rstrip('\n')
        if not line.strip() or line.startswith('#') or line.endswith(' textures selected'):
            continue
        f = line.split('\t')
        if len(f) >= 7:
            key, fmt, w, h, alpha, nbytes, names = f[0], f[1], int(f[2]), int(f[3]), int(f[4]), int(f[5]), f[6].split()
        else:  # '<key> <fmt> <w>x<h> names...': no alpha column: assume opaque (BC1)
            g = line.split()
            if len(g) < 4 or 'x' not in g[2]:
                continue
            key, fmt = g[0], g[1]
            w, h = (int(v) for v in g[2].split('x'))
            alpha, names = 0, g[3:]
            nbytes = pack_bytes(w * 2, h * 2, alpha)
        out[int(key, 16)] = dict(fmt=fmt, w=w, h=h, alpha=alpha, bytes=nbytes, names=names)
    return out


def pack_bytes(w, h, alpha):
    blk, tot = (16 if alpha else 8), 0
    while True:
        tot += ((w + 3) // 4) * ((h + 3) // 4) * blk
        if w == 1 and h == 1:
            return tot
        w, h = max(1, w // 2), max(1, h // 2)


def read_census(path):
    """(header dict, [row dict]) of one texmag file. Pixel columns are returned per picture."""
    hdr, cols, rows = {}, None, []
    for line in open(path):
        line = line.rstrip('\n')
        if line.startswith('# cols:'):
            cols = line[len('# cols:'):].split()
        elif line.startswith('#') or not line.strip():
            continue
        elif '=' in line and '\t' not in line:
            k, v = line.split('=', 1)
            hdr[k] = v
        elif cols:
            f = line.split('\t')
            if len(f) != len(cols):
                continue
            r = dict(zip(cols, f))
            rows.append(r)
    pics = max(1, int(hdr.get('pictures_sampled', '1') or 1))
    out = []
    for r in rows:
        d = dict(key=int(r['key'], 16), w=int(r['w']), h=int(r['h']), fmt=r['fmt'], player=int(r['player']), pics=int(r['pics']))
        for c in ('px_all', 'px_ge1', 'px_ge15', 'px_ge2', 'px_ge3', 'px_lod05', 'px_lod1', 'px_lod2'):
            d[c] = float(r[c]) / pics
        for c in ('mean_mag', 'mean_lod', 'mean_lod_min', 'max_mag', 'min_mag', 'peak_share_pct', 'avg_share_pct'):
            d[c] = float(r[c])
        out.append(d)
    hdr['_pictures'] = pics
    return hdr, out


def select(stages, lst, total_bytes, stage_bytes, min_area, include_player=False):
    """stages: {name: (hdr, rows)}. Returns (chosen {key: why}, rejected {key: reason}, unknown set of census keys not in the list)."""
    cand, unknown = {}, set()
    for name, (hdr, rows) in stages.items():
        for r in rows:
            k = r['key']
            if k not in lst:
                unknown.add(k)
                continue
            c = cand.setdefault(k, dict(key=k, px15={}, player=False, seen={}))
            c['seen'][name] = r['px_all']
            c['px15'][name] = r['px_ge15']
            c['player'] |= bool(r['player'])
    chosen, rejected = {}, {}
    ranked = []
    for k, c in cand.items():
        mag = max(c['px15'].values()) >= min_area
        ply = include_player and c['player'] and max(c['seen'].values()) >= min_area
        if not (mag or ply):
            rejected[k] = 'under %g magnified pixels in every stage' % min_area
            continue
        score = sum(c['px15'].values()) / float(lst[k]['bytes'])
        ranked.append((-(score if mag else 1e9), k, 'player' if (ply and not mag) else 'magnified'))
    ranked.sort()
    total = 0
    per = {n: 0 for n in stages}
    for _, k, why in ranked:
        b = lst[k]['bytes']
        in_stages = [n for n, v in cand[k]['seen'].items() if v > 0]
        if total + b > total_bytes:
            rejected[k] = 'total budget'
            continue
        over = [n for n in in_stages if per[n] + b > stage_bytes]
        if over:
            rejected[k] = 'stage budget (%s)' % over[0]
            continue
        total += b
        for n in in_stages:
            per[n] += b
        chosen[k] = why
    return chosen, rejected, unknown, cand, per, total


def orig_bytes(e):
    return int(e['w'] * e['h'] * ORIG_BPP.get(e['fmt'], 2.0) * MIPS)


def report(stages, lst, chosen, rejected, unknown, cand, per, total, args):
    L = []
    L.append('HD texture selection from the magnification census')
    L.append('rule: 2x only where magnified >= 1.5 over >= %g display pixels of a picture (some stage); 4x never' % args.min_area)
    L.append('budgets: pack %.1f MiB, GPU per stage %.1f MiB%s' % (args.total_mb, args.stage_mb, '; player textures included whatever their magnification' if args.include_player else ''))
    L.append('')
    n_hd = sum(lst[k]['bytes'] for k in chosen)
    n_orig = sum(orig_bytes(lst[k]) for k in chosen)
    L.append('selected %d textures: pack %.2f MiB (BC1/BC3 with mips), roughly %.2f MiB more GPU memory than the originals (%.2f MiB estimated)' %
             (len(chosen), n_hd / 1048576.0, (n_hd - n_orig) / 1048576.0, n_orig / 1048576.0))
    L.append('census textures not in the list (format, size or archive not covered): %d; candidates left out: %d' % (len(unknown), len(rejected)))
    reasons = {}
    for r in rejected.values():
        reasons[r.split(' (')[0]] = reasons.get(r.split(' (')[0], 0) + 1
    for r, n in sorted(reasons.items()):
        L.append('   left out, %s: %d' % (r, n))
    L.append('')
    L.append('%-10s %9s %8s %10s %10s %10s %10s  %s' % ('stage', 'pictures', 'chosen', 'HD MiB', 'added MiB', 'sharp px', 'of textured', 'player textures (zoom runs: one stage file each)'))
    for name, (hdr, rows) in sorted(stages.items()):
        ks = [r for r in rows if r['key'] in chosen]
        hd = sum(lst[r['key']]['bytes'] for r in ks)
        og = sum(orig_bytes(lst[r['key']]) for r in ks)
        sharp = sum(r['px_ge15'] for r in ks)
        tex = float(hdr.get('textured_px_per_picture', 0) or 0)
        ply = [r for r in rows if r['player']]
        pl = ' '.join('%016x:mag%.2f/lod%+.2f' % (r['key'], r['mean_mag'], r['mean_lod']) for r in ply[:4]) + (' ...' if len(ply) > 4 else '')
        L.append('%-10s %9d %8d %10.2f %10.2f %10.0f %9.1f%%  %s' % (name, hdr['_pictures'], len(ks), hd / 1048576.0, (hd - og) / 1048576.0, sharp,
                                                                 100.0 * sharp / tex if tex else 0.0, pl))
    L.append('')
    L.append('PLAYER textures (flagged by the census from the HD pack\'s list; none flagged when no pack was loaded). Share of each one\'s pixels by the mip of a 2x')
    L.append('replacement the GPU would sample (HD mip = original lod + 1; mip 0 is the full 2x level; mean_mag > 1 = magnified, < 1 = minified):')
    L.append('%-10s %-16s %9s %7s %8s %7s %7s %7s %7s %s' % ('stage', 'key', 'px/pic', 'mag', 'lod', 'HD m0', 'HD m1', 'HD m2', 'HD m3+', 'archive:texture'))
    anyp = False
    for name, (hdr, rows) in sorted(stages.items()):
        for r in rows:
            if not r['player'] or r['px_all'] <= 0:
                continue
            anyp = True
            A = r['px_all']
            m0 = r['px_ge2'] / A
            m3 = r['px_lod2'] / A
            m2 = (r['px_lod1'] - r['px_lod2']) / A
            m1 = max(0.0, 1.0 - m0 - m2 - m3)
            nm = ' '.join(lst[r['key']]['names'][:2]) if r['key'] in lst else '(not in the list)'
            L.append('%-10s %016x %9.0f %7.2f %+8.2f %6.0f%% %6.0f%% %6.0f%% %6.0f%% %s' % (name, r['key'], A, r['mean_mag'], r['mean_lod'], 100 * m0, 100 * m1, 100 * m2, 100 * m3, nm))
    if not anyp:
        L.append('(no player texture in these census files)')
    L.append('')
    L.append('selected, best first (pixels per picture magnified >= 1.5, in the stage where most; GPU bytes of the 2x entry):')
    for k in sorted(chosen, key=lambda k: -max(cand[k]['px15'].values())):
        e = lst[k]
        L.append('  %016x %-6s %4dx%-4d %s  px15 %.0f  %d B  [%s]  %s' % (k, e['fmt'], e['w'], e['h'], 'BC3' if e['alpha'] else 'BC1', max(cand[k]['px15'].values()),
                                                                       e['bytes'], chosen[k], ' '.join(e['names'][:3])))
    return '\n'.join(L) + '\n'


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('census', nargs='+', help='texmag-<stagekey>.txt files (globs allowed)')
    ap.add_argument('--list', required=True, help="build_pack.py --list-all output")
    ap.add_argument('--total-mb', type=float, default=48.0, help='budget for the whole pack (MiB of BC1/BC3 with mips)')
    ap.add_argument('--stage-mb', type=float, default=20.0, help='budget for the HD bytes resident in one stage (MiB)')
    ap.add_argument('--min-area', type=float, default=2000.0, help='A: display pixels per picture magnified >= 1.5 a texture must cover (some stage)')
    ap.add_argument('--include-player', action='store_true')
    ap.add_argument('--out', default='selection-census.txt')
    ap.add_argument('--report', default=None)
    a = ap.parse_args(argv)
    files = []
    for p in a.census:
        files += sorted(glob.glob(p)) or [p]
    stages = {}
    for p in files:
        hdr, rows = read_census(p)
        stages[os.path.basename(p).replace('texmag-', '').replace('.txt', '')] = (hdr, rows)
    lst = read_list(a.list)
    chosen, rejected, unknown, cand, per, total = select(stages, lst, int(a.total_mb * 1048576), int(a.stage_mb * 1048576), a.min_area, a.include_player)
    with open(a.out, 'w') as f:
        f.write('# census selection: %d textures, %.2f MiB pack. build_pack.py --select-keys this file\n' % (len(chosen), total / 1048576.0))
        for k in sorted(chosen, key=lambda k: lst[k]['names'][0]):
            e = lst[k]
            f.write('%016x  # %s %dx%d %s\n' % (k, e['fmt'], e['w'], e['h'], ' '.join(e['names'][:3])))
    text = report(stages, lst, chosen, rejected, unknown, cand, per, total, a)
    if a.report:
        open(a.report, 'w').write(text)
    sys.stdout.write(text)
    return 0


if __name__ == '__main__':
    sys.exit(main())
