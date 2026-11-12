#!/usr/bin/env python3
"""Build resources/pagemade.ico from resources/pagemade-icon.png.

    python3 resources/make_pagemade_icon.py

The png is the app icon. This writes the .ico the Windows executable
(resources/pagemade.rc) and the NSIS installer both use. The png may be
portrait; the center square is kept, the same crop the Mac .icns uses.
Rerun this after replacing the png. The .ico is checked in so a Windows
build does not need Pillow.

Pillow is required: pip install Pillow.
"""

import os
import sys

try:
    from PIL import Image
except ImportError:
    sys.exit("Pillow is required: pip install Pillow")

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PNG = os.path.join(ROOT, "resources", "pagemade-icon.png")
ICO = os.path.join(ROOT, "resources", "pagemade.ico")

# Sizes Explorer asks for. bitmap_format="bmp" matches resources/make_icons.py:
# PNG-compressed ICO entries trip some NSIS versions.
ICO_SIZES = [16, 24, 32, 48, 64, 128, 256]


def center_square(im):
    w, h = im.size
    side = min(w, h)
    left = (w - side) // 2
    top = (h - side) // 2
    return im.crop((left, top, left + side, top + side))


def main():
    if not os.path.exists(PNG):
        sys.exit("missing %s" % PNG)
    src = Image.open(PNG).convert("RGBA")
    master = center_square(src)
    print("read %s (%dx%d), square %dx%d" % (
        PNG, src.size[0], src.size[1], master.size[0], master.size[1]))

    master.save(ICO, format="ICO", sizes=[(s, s) for s in ICO_SIZES],
                bitmap_format="bmp")
    print("wrote %s (%d sizes)" % (ICO, len(ICO_SIZES)))


if __name__ == "__main__":
    main()
