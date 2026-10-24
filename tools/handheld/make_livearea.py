#!/usr/bin/env python3
"""Draws the Vita package's bubble pictures (original artwork: a starfield, a star, plain text) into
vita/vgx-game/sce_sys/: icon0.png 128x128, pic0.png 960x544, livearea/contents/bg.png 840x500 and
startup.png 280x158, all as 8-bit palette PNGs (the console's installer refuses anything else).
  python3 tools/handheld/make_livearea.py        (needs Pillow and the DejaVu fonts)"""
import math, os, random
from PIL import Image, ImageDraw, ImageFilter, ImageFont

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'vita', 'vgx-game', 'sce_sys')
BOLD = '/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf'
REG = '/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf'

def sky(w, h, seed):
    rnd = random.Random(seed)
    im = Image.new('RGB', (w, h))
    px = im.load()
    for y in range(h):
        for x in range(w):
            t = y / (h - 1)
            g = math.hypot((x - w * 0.62) / w, (y - h * 0.38) / h)
            glow = max(0.0, 1.0 - g * 2.2) ** 2
            px[x, y] = (int(8 + 22 * t + 70 * glow), int(10 + 14 * t + 40 * glow), int(38 + 52 * (1 - t) + 110 * glow))
    d = ImageDraw.Draw(im)
    for _ in range(w * h // 900):
        x, y = rnd.randrange(w), rnd.randrange(h)
        b = rnd.randrange(120, 256)
        r = 1 if rnd.random() < 0.85 else 2
        d.ellipse((x, y, x + r, y + r), fill=(b, b, min(255, b + 20)))
    return im

def star(size, colour=(255, 204, 40), edge=(150, 96, 0)):
    s = size * 4
    im = Image.new('RGBA', (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    c = s / 2
    pts = []
    for i in range(10):
        a = -math.pi / 2 + i * math.pi / 5
        r = s * 0.47 if i % 2 == 0 else s * 0.24
        pts.append((c + r * math.cos(a), c + r * math.sin(a)))
    d.polygon(pts, fill=edge)
    inner = [(c + (x - c) * 0.86, c + (y - c) * 0.86) for x, y in pts]
    d.polygon(inner, fill=colour)
    hi = [(c + (x - c) * 0.86, c + (y - c) * 0.86) for x, y in pts[9:] + pts[:2]] + [(c, c)]
    d.polygon(hi, fill=(255, 232, 120))  # one lit facet
    return im.resize((size, size), Image.LANCZOS)

def text(im, xy, s, font, fill=(255, 255, 255), shadow=(0, 0, 30)):
    d = ImageDraw.Draw(im)
    x, y = xy
    d.text((x + 2, y + 2), s, font=font, fill=shadow)
    d.text((x, y), s, font=font, fill=fill)

def save(im, name):
    path = os.path.join(OUT, name)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    im.convert('RGB').quantize(colors=256, method=Image.MEDIANCUT, dither=Image.FLOYDSTEINBERG).save(path, optimize=True)
    print(name, im.size, os.path.getsize(path), 'bytes')

def main():
    # icon
    im = sky(128, 128, 1)
    st = star(92)
    im.paste(st, (18, 8), st)
    f = ImageFont.truetype(BOLD, 21)
    w = ImageDraw.Draw(im).textlength('GALAXY', font=f)
    text(im, ((128 - w) / 2, 98), 'GALAXY', f)
    save(im, 'icon0.png')
    # splash shown while the application loads
    im = sky(960, 544, 2)
    st = star(220)
    im.paste(st, (370, 90), st)
    f = ImageFont.truetype(BOLD, 54)
    w = ImageDraw.Draw(im).textlength('Super Mario Galaxy', font=f)
    text(im, ((960 - w) / 2, 330), 'Super Mario Galaxy', f)
    f2 = ImageFont.truetype(REG, 24)
    s2 = 'native port for PlayStation Vita'
    w = ImageDraw.Draw(im).textlength(s2, font=f2)
    text(im, ((960 - w) / 2, 410), s2, f2, fill=(210, 220, 255))
    save(im, 'pic0.png')
    # LiveArea background and gate
    im = sky(840, 500, 3)
    st = star(170)
    im.paste(st, (60, 60), st)
    f = ImageFont.truetype(BOLD, 44)
    text(im, (260, 90), 'Super Mario Galaxy', f)
    f2 = ImageFont.truetype(REG, 22)
    for i, line in enumerate(['Native port for PlayStation Vita, version 1.0.',
                              'Touch, rear touch aiming and tilt controls.',
                              'Needs your own copy of the game.']):
        text(im, (262, 160 + i * 32), line, f2, fill=(215, 225, 255))
    save(im, 'livearea/contents/bg.png')
    im = sky(280, 158, 4)
    st = star(84)
    im.paste(st, (14, 34), st)
    f = ImageFont.truetype(BOLD, 30)
    text(im, (112, 60), 'Start', f)
    save(im, 'livearea/contents/startup.png')

main()
