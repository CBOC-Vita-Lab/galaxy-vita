#!/usr/bin/env python3
"""The Vita package's bubble and LiveArea pictures, made from the game's own title screen (sce_sys/loading-title.rgba,
the approved title picture in docs/ASSETS.md): pic0.png 960x544 and livearea bg.png 840x500 (the title with its two text lines
removed), startup.png 280x158 and icon0.png 128x128 (the title logo on its sky), settings.png 280x64 (the Settings tile).
All 8-bit palette PNGs (the console installer refuses others). Deterministic.
  python3 -I tools/handheld/make_livearea_title.py vita/vgx-game/sce_sys/loading-title.rgba <out-dir>
Needs Pillow, NumPy, OpenCV and the DejaVu fonts. The older generated starfield pictures: make_livearea.py."""
import sys, numpy as np
from PIL import Image, ImageFilter, ImageDraw, ImageFont
src, outdir = sys.argv[1], sys.argv[2]
im = Image.frombytes('RGBA', (960, 544), open(src, 'rb').read()).convert('RGB')
a = np.asarray(im).copy()
# 1) clean: inpaint only the pixels of the two text lines (rows 392..518, cols 222..738), keeping the sky around them
import cv2
y0, y1, x0, x1 = 392, 518, 222, 738
bgr = cv2.cvtColor(a, cv2.COLOR_RGB2BGR)
reg = a[y0:y1, x0:x1].astype(int)
txt = (reg.sum(2) > 330) | ((reg[..., 2] < reg[..., 0] + 30))      # white / grey text and its dark outline
m = np.zeros((544, 960), np.uint8); m[y0:y1, x0:x1][txt] = 255
m = cv2.dilate(m, np.ones((5, 5), np.uint8), iterations=2)
out_bgr = cv2.inpaint(bgr, m, 9, cv2.INPAINT_TELEA)
out = cv2.cvtColor(out_bgr, cv2.COLOR_BGR2RGB).astype(int)
# put back stars where text was: the same density and brightness as the sky beside it (deterministic)
side = np.concatenate([a[y0:y1, 0:x0], a[y0:y1, x1:960]], axis=1).astype(int)
lum = side.sum(2); base = np.median(lum)
stars = side[lum > base + 60]; density = len(stars) / lum.size
rng = np.random.default_rng(1985)
ys, xs = np.nonzero(m)
pick = rng.random(len(ys)) < density
for y, x, k in zip(ys[pick], xs[pick], rng.integers(0, max(1, len(stars)), pick.sum())):
    out[y, x] = np.maximum(out[y, x], stars[k])
clean = Image.fromarray(out.clip(0, 255).astype(np.uint8))
# logo box: bright, saturated pixels in rows 40..350
b = np.asarray(clean).astype(int); Rc, Gc, Bc = b[..., 0], b[..., 1], b[..., 2]
let = ~((Bc > Rc + 35) & (Bc > Gc + 10))   # the logo: everything that is not the blue sky
cols = np.where(let[20:380].sum(0) > 25)[0]; rows = np.where(let[:, cols.min():cols.max() + 1].sum(1) > 25)[0]
rows = rows[rows < 390]
L, R, T, B = cols.min(), cols.max(), rows.min(), rows.max()
print('logo box', L, R, T, B)
def p8(img): return img.convert('RGB').quantize(256, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.FLOYDSTEINBERG)
def cover(img, w, h):
    s = max(w / img.width, h / img.height); r = img.resize((round(img.width * s), round(img.height * s)), Image.LANCZOS)
    return r.crop(((r.width - w) // 2, (r.height - h) // 2, (r.width - w) // 2 + w, (r.height - h) // 2 + h))
def logo_on_sky(w, h, fill):
    pad = 14
    box = clean.crop((L - pad, T - pad, R + pad, B + pad))
    sky = cover(clean.crop((0, 380, 960, 544)), w, h)
    s = min(w * fill / box.width, h * fill / box.height)
    lg = box.resize((round(box.width * s), round(box.height * s)), Image.LANCZOS)
    m = Image.new('L', lg.size, 0); ImageDraw.Draw(m).rounded_rectangle((0, 0, lg.width - 1, lg.height - 1), radius=max(4, lg.width // 12), fill=255)
    sky.paste(lg, ((w - lg.width) // 2, (h - lg.height) // 2), m.filter(ImageFilter.GaussianBlur(max(2, lg.width // 40))))
    return sky
p8(clean).save(f'{outdir}/pic0.png', optimize=True)
p8(cover(clean, 840, 500)).save(f'{outdir}/bg.png', optimize=True)
p8(logo_on_sky(280, 158, 0.92)).save(f'{outdir}/startup.png', optimize=True)
p8(logo_on_sky(128, 128, 0.98)).save(f'{outdir}/icon0.png', optimize=True)
# The LiveArea "Settings" tile (280x64): the title's sky with the word in white, a thin dark outline
tile = cover(clean.crop((0, 380, 960, 544)), 280, 64)
d = ImageDraw.Draw(tile)
font = ImageFont.truetype('/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf', 30)
text = 'Settings'
tw = d.textlength(text, font=font)
d.text(((280 - tw) / 2, 13), text, font=font, fill=(255, 255, 255), stroke_width=2, stroke_fill=(8, 30, 60))
d.rounded_rectangle((1, 1, 278, 62), radius=10, outline=(255, 214, 64), width=2)
p8(tile).save(f'{outdir}/settings.png', optimize=True)
