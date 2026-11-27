#!/usr/bin/env python3
"""Contact sheets of the package pictures for choosing a style: one PNG per style with all five pictures as the console gets them
(read back from the 8-bit PNG files), the round home-screen bubble at 128 and 64 px, and a mock of the LiveArea page (the background,
the start gate picture with a Start button placeholder and the Settings tile). The mock's layout is approximate: see make_livearea.py
GATE / PEEL. Preview only (uses Pillow); the shipped pictures do not depend on it.
  python3 tools/handheld/livearea_preview.py <out-dir> [style ...]       (builds each style's pictures in <out-dir>/<style>/)"""
import importlib.util
import os
import sys

from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location('make_livearea', os.path.join(HERE, 'make_livearea.py'))
ML = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ML)
FONT = '/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf'


def bubble(icon, size):
    """The icon as the home screen shows it: inside a circle, with a thin light rim, on the home screen's wallpaper."""
    s = size * 4
    big = icon.convert('RGB').resize((s, s), Image.LANCZOS)
    mask = Image.new('L', (s, s), 0)
    ImageDraw.Draw(mask).ellipse((4, 4, s - 5, s - 5), fill=255)
    out = Image.new('RGB', (s + 40, s + 40), (60, 90, 130))
    out.paste(big, (20, 20), mask)
    ImageDraw.Draw(out).ellipse((22, 22, s + 17, s + 17), outline=(235, 240, 248), width=max(4, s // 40))
    return out.resize(((s + 40) // 4, (s + 40) // 4), Image.LANCZOS)


def livearea(bg, startup, settings):
    """A rough LiveArea page: status bar, the 840x500 page, the gate (startup picture and a Start button) and the Settings tile."""
    page = Image.new('RGB', (960, 544), (30, 32, 40))
    d = ImageDraw.Draw(page)
    d.rectangle((0, 0, 959, 29), fill=(12, 12, 16))
    page.paste(bg.convert('RGB'), (60, 34))
    f = ImageFont.truetype(FONT, 15)
    d.text((76, 42), 'Super Mario Galaxy', font=f, fill=(255, 255, 255))   # the title the system writes
    gx, gy = 60 + 280, 34 + 120
    page.paste(startup.convert('RGB'), (gx, gy))
    d.rounded_rectangle((gx + 60, gy + 166, gx + 220, gy + 210), radius=8, fill=(235, 235, 240))
    d.text((gx + 118, gy + 178), 'Start', font=f, fill=(20, 20, 20))
    page.paste(settings.convert('RGB'), (60 + 540, 34 + 410))           # an assumed place for frame3
    d.polygon([(900, 34), (900, 84), (850, 34)], fill=(220, 220, 225))    # the peel corner
    return page


def sheet(folder, style, out):
    pics = {n: Image.open(os.path.join(folder, n)) for n in ('icon0.png', 'pic0.png', 'livearea/contents/bg.png',
                                                            'livearea/contents/startup.png', 'livearea/contents/settings.png')}
    W, H = 960 + 20 + 960 + 40, 544 + 20 + 544 + 220
    c = Image.new('RGB', (W, H), (24, 24, 28))
    d = ImageDraw.Draw(c)
    f = ImageFont.truetype(FONT, 16)
    c.paste(pics['pic0.png'].convert('RGB'), (20, 40))
    d.text((20, 14), f'style "{style}"   pic0.png 960x544 (launch picture)', font=f, fill=(220, 220, 220))
    c.paste(livearea(pics['livearea/contents/bg.png'], pics['livearea/contents/startup.png'], pics['livearea/contents/settings.png']), (1000, 40))
    d.text((1000, 14), 'LiveArea mock: bg.png 840x500 + startup.png (gate) + settings.png (frame3, assumed place)', font=f, fill=(220, 220, 220))
    y = 604
    d.text((20, y - 24), 'bg.png 840x500', font=f, fill=(220, 220, 220))
    c.paste(pics['livearea/contents/bg.png'].convert('RGB'), (20, y))
    x = 880
    d.text((x, y - 24), 'startup.png 280x158', font=f, fill=(220, 220, 220))
    c.paste(pics['livearea/contents/startup.png'].convert('RGB'), (x, y))
    d.text((x, y + 170), 'settings.png 280x64', font=f, fill=(220, 220, 220))
    c.paste(pics['livearea/contents/settings.png'].convert('RGB'), (x, y + 194))
    x = 1180
    d.text((x, y - 24), 'icon0.png 128x128 (x2)', font=f, fill=(220, 220, 220))
    c.paste(pics['icon0.png'].convert('RGB').resize((256, 256), Image.NEAREST), (x, y))
    d.text((x + 280, y - 24), 'bubble 128 / 64 px', font=f, fill=(220, 220, 220))
    b128, b64 = bubble(pics['icon0.png'], 128), bubble(pics['icon0.png'], 64)
    c.paste(b128, (x + 280, y))
    c.paste(b64, (x + 280 + b128.width + 20, y + 40))
    c.paste(pics['icon0.png'].convert('RGB'), (x, y + 280))
    d.text((x + 140, y + 330), 'icon0 at 1:1', font=f, fill=(220, 220, 220))
    c.save(out)


def main():
    out = sys.argv[1]
    styles = sys.argv[2:] or list(ML.STYLES)
    for style in styles:
        folder = os.path.join(out, style)
        ML.main([folder, '--style', style])
        sheet(folder, style, os.path.join(out, f'sheet-{style}.png'))
        print('sheet', os.path.join(out, f'sheet-{style}.png'))


if __name__ == '__main__':
    main()
