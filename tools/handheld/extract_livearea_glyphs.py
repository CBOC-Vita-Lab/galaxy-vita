#!/usr/bin/env python3
"""One-off helper: writes tools/handheld/livearea_glyphs.py, the glyph outlines make_livearea.py sets its text with.
The outlines are a small subset (the Latin letters, digits and a few signs) of Inter 4.0 (Inter Display Bold for titles, Inter Medium
for small text) by The Inter Project Authors, under the SIL Open Font License 1.1 (licenses/Inter-OFL.txt). Curves are flattened
here into integer polygons in font units (2048 per em), and the pair kerning of the font is kept for those characters, so the
generator needs neither the font file nor a font rasteriser: same polygons, same pixels on every machine.
  python3 tools/handheld/extract_livearea_glyphs.py [InterDisplay-Bold.otf Inter-Medium.otf]     (needs fontTools)"""
import os
import sys
from fontTools.ttLib import TTFont
from fontTools.pens.basePen import BasePen

CHARS = ' ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789.,:-\'&/'
FACES = {'display': '/usr/share/fonts/opentype/inter/InterDisplay-Bold.otf', 'text': '/usr/share/fonts/opentype/inter/Inter-Medium.otf'}
if len(sys.argv) == 3:
    FACES = {'display': sys.argv[1], 'text': sys.argv[2]}


class Flatten(BasePen):
    def __init__(self, glyphset):
        super().__init__(glyphset)
        self.contours, self.cur = [], []

    def _moveTo(self, p):
        self.cur = [p]

    def _lineTo(self, p):
        self.cur.append(p)

    def _curveToOne(self, a, b, c):
        p0 = self.cur[-1]
        n = 10
        for i in range(1, n + 1):
            t = i / n
            u = 1 - t
            self.cur.append((u ** 3 * p0[0] + 3 * u * u * t * a[0] + 3 * u * t * t * b[0] + t ** 3 * c[0],
                             u ** 3 * p0[1] + 3 * u * u * t * a[1] + 3 * u * t * t * b[1] + t ** 3 * c[1]))

    def _qCurveToOne(self, a, b):
        p0 = self.cur[-1]
        n = 8
        for i in range(1, n + 1):
            t = i / n
            u = 1 - t
            self.cur.append((u * u * p0[0] + 2 * u * t * a[0] + t * t * b[0], u * u * p0[1] + 2 * u * t * a[1] + t * t * b[1]))

    def _closePath(self):
        pts = [(int(round(x)), int(round(y))) for x, y in self.cur]
        out = []
        for p in pts:
            if not out or out[-1] != p:
                out.append(p)
        if len(out) > 1 and out[0] == out[-1]:
            out.pop()
        if len(out) >= 3:
            self.contours.append(out)
        self.cur = []

    _endPath = _closePath


def kerning(font, glyph_names):
    """Pair kerning (x advance adjustments) of the GPOS 'kern' feature for the given glyphs."""
    gpos = font['GPOS'].table
    wanted = set()
    for rec in gpos.FeatureList.FeatureRecord:
        if rec.FeatureTag == 'kern':
            wanted.update(rec.Feature.LookupListIndex)
    pairs = {}
    names = set(glyph_names)
    for index in sorted(wanted):
        lookup = gpos.LookupList.Lookup[index]
        for sub in lookup.SubTable:
            if lookup.LookupType == 9:
                if sub.ExtensionLookupType != 2:
                    continue
                sub = sub.ExtSubTable
            elif lookup.LookupType != 2:
                continue
            cov = sub.Coverage.glyphs
            if sub.Format == 1:
                for i, first in enumerate(cov):
                    if first not in names:
                        continue
                    for pvr in sub.PairSet[i].PairValueRecord:
                        if pvr.SecondGlyph in names and pvr.Value1 is not None and getattr(pvr.Value1, 'XAdvance', 0):
                            pairs.setdefault((first, pvr.SecondGlyph), pvr.Value1.XAdvance)
            else:
                c1, c2 = sub.ClassDef1.classDefs, sub.ClassDef2.classDefs
                for first in cov:
                    if first not in names:
                        continue
                    row = sub.Class1Record[c1.get(first, 0)]
                    for second in names:
                        v = row.Class2Record[c2.get(second, 0)].Value1
                        if v is not None and getattr(v, 'XAdvance', 0):
                            pairs.setdefault((first, second), v.XAdvance)
    return pairs


def face(path):
    font = TTFont(path)
    cmap = font.getBestCmap()
    gs = font.getGlyphSet()
    glyphs, names = {}, {}
    for ch in CHARS:
        name = cmap[ord(ch)]
        names[name] = ch
        pen = Flatten(gs)
        gs[name].draw(pen)
        glyphs[ch] = (gs[name].width, pen.contours)
    kern = {names[a] + names[b]: v for (a, b), v in kerning(font, list(names)).items()}
    return glyphs, kern, font['OS/2'].sCapHeight, font['OS/2'].sxHeight


def main():
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'livearea_glyphs.py')
    lines = ['"""Glyph outlines for make_livearea.py: a subset of Inter 4.0 (Copyright 2016 The Inter Project Authors), SIL Open Font',
             'License 1.1, see licenses/Inter-OFL.txt. Made by extract_livearea_glyphs.py: curves flattened to integer polygons in font',
             'units (2048 per em, y up), pair kerning kept. Data only; no font file is shipped or read."""',
             'UNITS = 2048', 'FACES = {}']
    for key, path in FACES.items():
        glyphs, kern, cap, xh = face(path)
        lines.append(f'FACES[{key!r}] = {{"cap": {cap}, "xheight": {xh}, "kern": {dict(sorted(kern.items()))!r}, "glyphs": {{')
        for ch in CHARS:
            adv, contours = glyphs[ch]
            flat = [[c for p in contour for c in p] for contour in contours]
            lines.append(f'    {ch!r}: ({adv}, {flat!r}),')
        lines.append('}}')
    with open(out, 'w') as f:
        f.write('\n'.join(lines) + '\n')
    print(out, os.path.getsize(out), 'bytes')


main()
