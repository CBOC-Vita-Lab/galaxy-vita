#!/usr/bin/env python3
"""Button pictures for handhelds: the glyphs that replace the Wii controller pictures of the game's
picture font (platform/src/port/handheld_ui.cpp puts them over the font's texture sheets when a sheet is
decoded). Writes ../../platform/src/port/handheld_glyphs.inc and, with a path argument, a preview PNG.

Every glyph is drawn here from scratch (plain shapes and DejaVu letters); nothing is taken from the game.
A cell of the font is 34 x 35 pixels; the pictures keep to the 30 x 30 the originals use, bottom at row 31.
"""
import sys, os
from PIL import Image, ImageDraw, ImageFont

W, H, S = 34, 35, 8          # cell size, supersampling
CX, CY, R = 16.0, 16.5, 14.0   # button centre and radius, in cell pixels
FONT = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"

def canvas():
    im = Image.new("RGBA", (W * S, H * S), (0, 0, 0, 0))
    return im, ImageDraw.Draw(im)

def done(im):
    return im.resize((W, H), Image.LANCZOS)

def disc(d, r=R, fill=(52, 54, 62, 255), rim=(236, 238, 244, 255)):
    c = (CX * S, CY * S)
    d.ellipse([c[0] - r * S, c[1] - r * S, c[0] + r * S, c[1] + r * S], fill=rim)
    q = (r - 1.6) * S
    d.ellipse([c[0] - q, c[1] - q, c[0] + q, c[1] + q], fill=fill)
    # a soft highlight on the upper half, as the game's own buttons have
    hi = Image.new("RGBA", (W * S, H * S), (0, 0, 0, 0))
    hd = ImageDraw.Draw(hi)
    hd.ellipse([c[0] - q * 0.78, c[1] - q * 0.92, c[0] + q * 0.78, c[1] + q * 0.1], fill=(255, 255, 255, 34))
    return hi

def face(symbol):
    im, d = canvas()
    hi = disc(d)
    c = (CX * S, CY * S)
    w = int(2.6 * S)
    if symbol == "cross":
        col, a = (124, 166, 255, 255), 6.2 * S
        d.line([c[0] - a, c[1] - a, c[0] + a, c[1] + a], fill=col, width=w)
        d.line([c[0] - a, c[1] + a, c[0] + a, c[1] - a], fill=col, width=w)
    elif symbol == "circle":
        col, a = (255, 104, 104, 255), 7.4 * S
        d.ellipse([c[0] - a, c[1] - a, c[0] + a, c[1] + a], outline=col, width=w)
    elif symbol == "triangle":
        col, a = (72, 226, 168, 255), 8.2 * S
        pts = [(c[0], c[1] - a), (c[0] + a * 0.92, c[1] + a * 0.62), (c[0] - a * 0.92, c[1] + a * 0.62)]
        d.line(pts + [pts[0], pts[1]], fill=col, width=w, joint="curve")
    elif symbol == "square":
        col, a = (240, 150, 222, 255), 6.6 * S
        d.rectangle([c[0] - a, c[1] - a, c[0] + a, c[1] + a], outline=col, width=w)
    im.alpha_composite(hi)
    return done(im)

def shoulder(letter):
    im, d = canvas()
    x0, y0, x1, y1 = 2.0 * S, 6.0 * S, 30.0 * S, 27.0 * S
    d.rounded_rectangle([x0, y0, x1, y1], radius=7 * S, fill=(236, 238, 244, 255))
    d.rounded_rectangle([x0 + 1.6 * S, y0 + 1.6 * S, x1 - 1.6 * S, y1 - 1.6 * S], radius=5.6 * S, fill=(52, 54, 62, 255))
    f = ImageFont.truetype(FONT, int(15.5 * S))
    bb = d.textbbox((0, 0), letter, font=f)
    d.text(((x0 + x1) / 2 - (bb[0] + bb[2]) / 2, (y0 + y1) / 2 - (bb[1] + bb[3]) / 2), letter, font=f, fill=(250, 250, 252, 255))
    return done(im)

def pill(text):
    im, d = canvas()
    x0, y0, x1, y1 = 0.6 * S, 9.5 * S, 31.4 * S, 23.5 * S
    d.rounded_rectangle([x0, y0, x1, y1], radius=7 * S, fill=(236, 238, 244, 255))
    d.rounded_rectangle([x0 + 1.4 * S, y0 + 1.4 * S, x1 - 1.4 * S, y1 - 1.4 * S], radius=5.6 * S, fill=(52, 54, 62, 255))
    size = 7.4 if len(text) <= 5 else 6.3
    f = ImageFont.truetype(FONT, int(size * S))
    bb = d.textbbox((0, 0), text, font=f)
    d.text(((x0 + x1) / 2 - (bb[0] + bb[2]) / 2, (y0 + y1) / 2 - (bb[1] + bb[3]) / 2), text, font=f, fill=(250, 250, 252, 255))
    return done(im)

def handheld():
    """The console itself, for "tilt ..." and "hold ... up"."""
    im, d = canvas()
    x0, y0, x1, y1 = 0.5 * S, 8.5 * S, 31.5 * S, 24.5 * S
    d.rounded_rectangle([x0, y0, x1, y1], radius=7.5 * S, fill=(236, 238, 244, 255))
    d.rounded_rectangle([x0 + 1.3 * S, y0 + 1.3 * S, x1 - 1.3 * S, y1 - 1.3 * S], radius=6.3 * S, fill=(40, 42, 50, 255))
    d.rectangle([8.2 * S, 11.4 * S, 23.8 * S, 21.6 * S], fill=(120, 190, 255, 255))      # screen
    d.ellipse([3.0 * S, 13.0 * S, 6.6 * S, 16.6 * S], fill=(200, 204, 212, 255))          # D-pad
    d.ellipse([25.4 * S, 13.0 * S, 29.0 * S, 16.6 * S], fill=(200, 204, 212, 255))        # buttons
    d.ellipse([4.0 * S, 18.4 * S, 6.4 * S, 20.8 * S], fill=(150, 154, 164, 255))          # sticks
    d.ellipse([25.6 * S, 18.4 * S, 28.0 * S, 20.8 * S], fill=(150, 154, 164, 255))
    return done(im)

def face_texture(symbol, size):
    """A face button as a whole square texture (the layouts' own button pictures)."""
    global W, H, CX, CY, R
    keep = (W, H, CX, CY, R)
    W = H = size
    CX = CY = size / 2.0
    R = size * 0.46
    try:
        im, d = canvas()
        hi = disc(d)
        c = (CX * S, CY * S)
        w = int(size * 0.085 * S)
        if symbol == "cross":
            col, a = (124, 166, 255, 255), size * 0.2 * S
            d.line([c[0] - a, c[1] - a, c[0] + a, c[1] + a], fill=col, width=w)
            d.line([c[0] - a, c[1] + a, c[0] + a, c[1] - a], fill=col, width=w)
        im.alpha_composite(hi)
        return done(im)
    finally:
        W, H, CX, CY, R = keep

# Whole textures: name, size, picture
TEXTURES = [("Cross48", 48, face_texture("cross", 48)), ("Cross40", 40, face_texture("cross", 40))]

# name -> picture; handheld_ui.cpp places them by the font's glyph index
GLYPHS = [
    ("Cross", face("cross")), ("Circle", face("circle")), ("Triangle", face("triangle")), ("Square", face("square")),
    ("L", shoulder("L")), ("R", shoulder("R")), ("Start", pill("START")), ("Select", pill("SELECT")), ("Console", handheld()),
]

def main():
    here = os.path.dirname(os.path.abspath(__file__))
    out = os.path.join(here, "..", "..", "platform", "src", "port", "handheld_glyphs.inc")
    with open(out, "w") as f:
        f.write("// Generated by tools/handheld/make_glyphs.py: do not edit. %d x %d RGBA pictures, top row first.\n" % (W, H))
        f.write("constexpr int kGlyphW = %d, kGlyphH = %d;\n" % (W, H))
        for name, im in GLYPHS:
            px = im.tobytes()
            f.write("static const unsigned char kGlyph%s[%d] = {\n" % (name, len(px)))
            for i in range(0, len(px), 32):
                f.write("    " + ",".join(str(b) for b in px[i:i + 32]) + ",\n")
            f.write("};\n")
        for name, size, im in TEXTURES:
            px = im.tobytes()
            f.write("static const unsigned char kTex%s[%d] = {\n" % (name, len(px)))
            for i in range(0, len(px), 32):
                f.write("    " + ",".join(str(b) for b in px[i:i + 32]) + ",\n")
            f.write("};\n")
    if len(sys.argv) > 1:
        prev = Image.new("RGBA", (len(GLYPHS) * (W * 3 + 6), H * 3 * 2 + 12), (240, 240, 235, 255))
        d = ImageDraw.Draw(prev)
        d.rectangle([0, H * 3 + 6, prev.width, prev.height], fill=(20, 40, 90, 255))
        for i, (name, im) in enumerate(GLYPHS):
            big = im.resize((W * 3, H * 3), Image.NEAREST)
            prev.alpha_composite(big, (i * (W * 3 + 6) + 3, 3))
            prev.alpha_composite(big, (i * (W * 3 + 6) + 3, H * 3 + 9))
        prev.save(sys.argv[1])

if __name__ == "__main__":
    main()
