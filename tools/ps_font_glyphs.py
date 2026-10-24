#!/usr/bin/env python3
"""Redraw the Xbox 360 button glyphs of the HUD's font atlases (the 512x512
textures whose first row has Y, B, RB, LB, ... as white shapes with their
letters cut out) as DualSense ones, in the same style.

usage: ps_font_glyphs.py ATLAS.png HASH OUT.png FONT.ttf

ATLAS.png is the decoded atlas (xtex.py decode), HASH which one it is (its
glyphs' places differ). Y and B become a triangle and a circle cut out of
their discs, the shoulder buttons' letters R1/L1/L2, and the START/BACK
labels OPTIONS/CREATE. The rest of the atlas is left as it is (then
make_texture_patches.py keeps only the blocks changed).
"""
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFont

SS = 8  # supersampling

# Per atlas: (kind, left, top, right, bottom) in texels, inclusive.
GLYPHS = {
    "B9A2193D67E467E4": [
        ("triangle", 60, 1, 89, 29), ("circle", 92, 1, 121, 29),
        ("R1", 124, 1, 157, 23), ("L1", 160, 1, 193, 23),
        ("OPTIONS", 194, 1, 205, 28), ("CREATE", 226, 1, 237, 28),
    ],
    "F1EE74C6F720024C": [
        ("triangle", 60, 1, 89, 29), ("circle", 92, 1, 121, 29),
        ("L2", 124, 1, 155, 25), ("R1", 158, 1, 191, 23), ("L1", 194, 1, 227, 23),
        ("OPTIONS", 228, 1, 239, 28), ("CREATE", 260, 1, 271, 28),
    ],
}


def filled(alpha):
    """The glyph's shape with its cut-outs filled: what the border can't
    reach through clear texels."""
    solid = alpha > 100
    h, w = solid.shape
    outside = np.zeros_like(solid)
    stack = [(y, x) for y in range(h) for x in (0, w - 1)] + \
            [(y, x) for x in range(w) for y in (0, h - 1)]
    while stack:
        y, x = stack.pop()
        if 0 <= y < h and 0 <= x < w and not outside[y, x] and not solid[y, x]:
            outside[y, x] = True
            stack += [(y + 1, x), (y - 1, x), (y, x + 1), (y, x - 1)]
    return np.where(outside, alpha, 255).astype(np.float32) / 255.0


def cut(size, draw_cut):
    """A cut-out mask (1 = clear) drawn at SS times, brought down."""
    w, h = size
    mask = Image.new("L", (w * SS, h * SS), 0)
    draw_cut(ImageDraw.Draw(mask), w * SS, h * SS)
    return np.asarray(mask.resize((w, h), Image.LANCZOS), np.float32) / 255.0


def text_fit(font_path, text, width, height):
    """A bold font size whose text fills width x height (in SS texels)."""
    size = height
    while size > 4:
        font = ImageFont.truetype(font_path, size)
        box = font.getbbox(text)
        if box[2] - box[0] <= width and box[3] - box[1] <= height:
            return font, box
        size -= 1
    font = ImageFont.truetype(font_path, 4)
    return font, font.getbbox(text)


def main():
    atlas_path, atlas, out_path, font_path = sys.argv[1:5]
    image = Image.open(atlas_path).convert("RGBA")
    pixels = np.asarray(image).astype(np.float32)
    for kind, x0, y0, x1, y1 in GLYPHS[atlas]:
        region = pixels[y0:y1 + 1, x0:x1 + 1]
        w, h = x1 - x0 + 1, y1 - y0 + 1
        if kind in ("OPTIONS", "CREATE"):
            # A label written down the glyph, as START/BACK were.
            label = Image.new("L", (h * SS, w * SS), 0)
            font, box = text_fit(font_path, kind, h * SS, w * SS * 0.8)
            ImageDraw.Draw(label).text(((h * SS - (box[2] - box[0])) / 2 - box[0],
                                        (w * SS - (box[3] - box[1])) / 2 - box[1]),
                                       kind, font=font, fill=255)
            label = label.rotate(-90, expand=True).resize((w, h), Image.LANCZOS)
            alpha = np.asarray(label, np.float32) / 255.0
        else:
            shape = filled(region[:, :, 3])

            def draw_cut(d, W, H, kind=kind):
                if kind == "triangle":
                    cx, cy, r = W / 2, H * 0.53, min(W, H) * 0.27
                    d.polygon([(cx, cy - r), (cx + r * 0.95, cy + r * 0.62),
                               (cx - r * 0.95, cy + r * 0.62)], outline=255, width=int(W * 0.09))
                elif kind == "circle":
                    r = min(W, H) * 0.25
                    d.ellipse([W / 2 - r, H / 2 - r, W / 2 + r, H / 2 + r], outline=255,
                              width=int(W * 0.09))
                else:
                    font, box = text_fit(font_path, kind, W * 0.62, H * 0.58)
                    d.text(((W - (box[2] - box[0])) / 2 - box[0],
                            (H - (box[3] - box[1])) / 2 - box[1] - H * 0.04),
                           kind, font=font, fill=255)

            alpha = shape * (1.0 - cut((w, h), draw_cut))
        region[:, :, :3] = 255.0
        region[:, :, 3] = alpha * 255.0
    Image.fromarray(np.clip(pixels, 0, 255).astype(np.uint8), "RGBA").save(out_path)


if __name__ == "__main__":
    main()
