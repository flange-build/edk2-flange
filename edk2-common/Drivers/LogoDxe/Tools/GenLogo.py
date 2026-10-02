#!/usr/bin/env python3
#
# Makes the boot logo bitmaps from the flange logo artwork
# (flange repository, docs/assets/flange-logo.png).
#
# The artwork is dark grey ink and an orange accent on a near-white page.
# The boot screen is black, so every pixel is split into its ink, accent and
# page parts, and put back together with white ink on a black page. Edges
# keep their anti-aliasing that way. The result is cropped to the artwork
# and written at the widths LogoDxe picks from.
#
# Copyright (c) 2026, edk2-flange contributors.
#
# SPDX-License-Identifier: BSD-2-Clause-Patent
#
# usage: GenLogo.py <flange-logo.png> [output directory]
#

import os
import sys

from PIL import Image

PAGE = (254, 254, 254)
INK = (36, 41, 47)
ACCENT = (253, 83, 43)

NEW_INK = (255, 255, 255)
NEW_PAGE = (0, 0, 0)

# Widths LogoDxe chooses from, as in Logo.idf.
WIDTHS = {"Logo-384.bmp": 384, "Logo-768.bmp": 768, "Logo-1152.bmp": 1152}

# Blank border kept around the artwork, in pixels of the source.
MARGIN = 8


def unmix(pixel):
    """Returns the ink and accent coverage (0..1) of a pixel."""
    q = [pixel[i] - PAGE[i] for i in range(3)]
    d = [INK[i] - PAGE[i] for i in range(3)]
    o = [ACCENT[i] - PAGE[i] for i in range(3)]
    dd = sum(x * x for x in d)
    oo = sum(x * x for x in o)
    do = sum(d[i] * o[i] for i in range(3))
    dq = sum(d[i] * q[i] for i in range(3))
    oq = sum(o[i] * q[i] for i in range(3))
    det = dd * oo - do * do
    ink = (dq * oo - oq * do) / det
    accent = (oq * dd - dq * do) / det
    ink = max(0.0, ink)
    accent = max(0.0, accent)
    total = ink + accent
    if total > 1.0:
        ink /= total
        accent /= total
    # Page noise of a level or two is not artwork.
    if ink + accent < 0.02:
        return 0.0, 0.0
    return ink, accent


def recolour(image):
    src = image.convert("RGB")
    width, height = src.size
    out = Image.new("RGB", src.size)
    sp = src.load()
    op = out.load()
    box = [width, height, 0, 0]
    cache = {}
    for y in range(height):
        for x in range(width):
            pixel = sp[x, y]
            if pixel not in cache:
                ink, accent = unmix(pixel)
                page = 1.0 - ink - accent
                cache[pixel] = (
                    tuple(
                        round(ink * NEW_INK[i] + accent * ACCENT[i] + page * NEW_PAGE[i])
                        for i in range(3)
                    ),
                    ink + accent >= 0.5,
                )
            colour, solid = cache[pixel]
            op[x, y] = colour
            if solid:
                box = [min(box[0], x), min(box[1], y), max(box[2], x), max(box[3], y)]
    left = max(0, box[0] - MARGIN)
    top = max(0, box[1] - MARGIN)
    right = min(width, box[2] + 1 + MARGIN)
    bottom = min(height, box[3] + 1 + MARGIN)
    return out.crop((left, top, right, bottom))


def main():
    if len(sys.argv) not in (2, 3):
        sys.exit(__doc__ or "usage: GenLogo.py <flange-logo.png> [output directory]")
    outdir = sys.argv[2] if len(sys.argv) == 3 else os.path.join(os.path.dirname(__file__), "..")
    logo = recolour(Image.open(sys.argv[1]))
    for name, width in WIDTHS.items():
        height = round(logo.height * width / logo.width)
        logo.resize((width, height), Image.LANCZOS).save(os.path.join(outdir, name))
        print(f"{name}: {width}x{height}")


if __name__ == "__main__":
    main()
