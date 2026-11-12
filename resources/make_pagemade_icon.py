#!/usr/bin/env python3
"""Build resources/pagemade.ico from resources/pagemade-logo.png.

    python3 resources/make_pagemade_icon.py
    python3 resources/make_pagemade_icon.py --draw

The png is the picture to replace. This writes the .ico the Windows
executable (resources/pagemade.rc) and the NSIS installer both use.
--draw overwrites the png with the built-in placeholder first.

Pillow is required to read a replaced png: pip install Pillow.
"""

import os
import sys

try:
    from PIL import Image, ImageDraw
except ImportError:
    sys.exit("Pillow is required: pip install Pillow")

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PNG = os.path.join(ROOT, "resources", "pagemade-logo.png")
ICO = os.path.join(ROOT, "resources", "pagemade.ico")

# Sizes Explorer asks for. bitmap_format="bmp" matches resources/make_icons.py:
# PNG-compressed ICO entries trip some NSIS versions.
ICO_SIZES = [16, 24, 32, 48, 64, 128, 256]


def draw_placeholder(size=1024):
    """A page with a block and a few rules. Meant to be replaced."""
    im = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    ink = (36, 62, 90, 255)
    inset = round(size * 0.04)
    d.rounded_rectangle(
        [inset, inset, size - 1 - inset, size - 1 - inset],
        radius=round(size * 0.18),
        fill=ink)
    m = round(size * 0.16)
    x0, y0, x1, y1 = m, m, size - 1 - m, size - 1 - m
    d.rectangle([x0, y0, x1, y1], fill=(247, 244, 238, 255))
    fold = round((x1 - x0) * 0.22)
    d.polygon([(x1 - fold, y0), (x1, y0), (x1, y0 + fold)],
              fill=(228, 221, 210, 255))

    pw, ph = x1 - x0, y1 - y0

    def px(f):
        return x0 + round(pw * f)

    def py(f):
        return y0 + round(ph * f)

    d.rounded_rectangle(
        [px(0.10), py(0.16), px(0.46), py(0.48)],
        radius=max(2, size // 80),
        fill=(208, 106, 58, 255))
    bar = max(3, round(ph * 0.045))
    for y, x2 in ((0.18, 0.88), (0.30, 0.80), (0.42, 0.70)):
        d.rectangle([px(0.54), py(y), px(x2), py(y) + bar], fill=ink)
    rule = max(3, round(ph * 0.035))
    d.rectangle([px(0.10), py(0.62), px(0.90), py(0.62) + rule], fill=ink)
    d.rectangle([px(0.10), py(0.74), px(0.58), py(0.74) + rule], fill=ink)
    d.rectangle([px(0.10), py(0.84), px(0.76), py(0.84) + rule], fill=ink)
    return im


def main():
    draw = "--draw" in sys.argv[1:]
    if draw or not os.path.exists(PNG):
        master = draw_placeholder()
        master.save(PNG, format="PNG", optimize=True)
        print("wrote %s (placeholder)" % PNG)
    else:
        master = Image.open(PNG).convert("RGBA")
        print("read %s (%dx%d)" % (PNG, master.size[0], master.size[1]))

    master.save(ICO, format="ICO", sizes=[(s, s) for s in ICO_SIZES],
                bitmap_format="bmp")
    print("wrote %s (%d sizes)" % (ICO, len(ICO_SIZES)))


if __name__ == "__main__":
    main()
