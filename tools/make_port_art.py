#!/usr/bin/env python3
"""Draw the port's own art (no game or publisher artwork): the PS5 icon, a
3840x2160 background and a transparent logo, from the Noto Sans fonts.

usage: make_port_art.py FONT_DIR OUT_DIR

Writes icon0.png (512x512), background.png, logo.png and splash.png
(background with the logo, for store pages) into OUT_DIR; then
`tools/make_ps5_art.py OUT_DIR/background.png OUT_DIR/logo.png` makes
assets/ps5/pic0.dds and pic1.dds.
"""
import math
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter, ImageFont

TOP, BOTTOM = (18, 30, 44), (4, 8, 14)
ACCENT = (110, 200, 235)


def gradient(width, height):
    image = Image.new("RGB", (width, height))
    draw = ImageDraw.Draw(image)
    for y in range(height):
        t = y / (height - 1)
        draw.line([(0, y), (width, y)],
                  fill=tuple(int(a + (b - a) * t) for a, b in zip(TOP, BOTTOM)))
    return image


def grid(image, step, alpha):
    """A faint tactical grid and a glow."""
    w, h = image.size
    overlay = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    draw = ImageDraw.Draw(overlay)
    for x in range(0, w, step):
        draw.line([(x, 0), (x, h)], fill=ACCENT + (alpha,), width=1)
    for y in range(0, h, step):
        draw.line([(0, y), (w, y)], fill=ACCENT + (alpha,), width=1)
    glow = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    ImageDraw.Draw(glow).ellipse([w * 0.45, h * 0.05, w * 1.15, h * 1.0], fill=ACCENT + (60,))
    glow = glow.filter(ImageFilter.GaussianBlur(w // 12))
    return Image.alpha_composite(Image.alpha_composite(image.convert("RGBA"), glow), overlay)


def reticle(draw, cx, cy, r, width, color):
    draw.ellipse([cx - r, cy - r, cx + r, cy + r], outline=color, width=width)
    inner = r * 0.55
    draw.ellipse([cx - inner, cy - inner, cx + inner, cy + inner], outline=color, width=width)
    for angle in range(0, 360, 90):
        a = math.radians(angle)
        x0, y0 = cx + math.cos(a) * r * 0.25, cy + math.sin(a) * r * 0.25
        x1, y1 = cx + math.cos(a) * r * 1.25, cy + math.sin(a) * r * 1.25
        draw.line([(x0, y0), (x1, y1)], fill=color, width=width)
    d = max(2, width)
    draw.ellipse([cx - d, cy - d, cx + d, cy + d], fill=color)


def logo(fonts, width):
    """The title as text, with a reticle, on transparency."""
    bold = ImageFont.truetype(str(fonts / "NotoSans-Bold.ttf"), int(width * 0.115))
    regular = ImageFont.truetype(str(fonts / "NotoSans-Regular.ttf"), int(width * 0.052))
    small = ImageFont.truetype(str(fonts / "NotoSans-Bold.ttf"), int(width * 0.03))
    height = int(width * 0.42)
    image = Image.new("RGBA", (width, height), (0, 0, 0, 0))
    draw = ImageDraw.Draw(image)
    draw.text((0, 0), "GHOST RECON", font=bold, fill=(235, 242, 248, 255))
    y = int(width * 0.16)
    draw.text((int(width * 0.004), y), "F U T U R E   S O L D I E R", font=regular,
              fill=ACCENT + (255,))
    y += int(width * 0.095)
    draw.rectangle([0, y, int(width * 0.62), y + max(3, width // 500)], fill=ACCENT + (255,))
    draw.text((0, y + int(width * 0.02)), "PS5 NATIVE PORT  ·  FAN PROJECT", font=small,
              fill=(170, 185, 200, 255))
    return image


def main():
    fonts, out = Path(sys.argv[1]), Path(sys.argv[2])
    out.mkdir(parents=True, exist_ok=True)

    background = grid(gradient(3840, 2160), 120, 18)
    draw = ImageDraw.Draw(background)
    reticle(draw, 2780, 1080, 520, 10, ACCENT + (150,))
    background.convert("RGB").save(out / "background.png")

    title = logo(fonts, 1900)
    title.save(out / "logo.png")
    splash = background.copy()
    splash.alpha_composite(title, (260, (2160 - title.height) // 2))
    splash.convert("RGB").save(out / "splash.png")

    icon = grid(gradient(512, 512), 32, 22)
    draw = ImageDraw.Draw(icon)
    reticle(draw, 256, 214, 120, 6, ACCENT + (255,))
    bold = ImageFont.truetype(str(fonts / "NotoSans-Bold.ttf"), 72)
    small = ImageFont.truetype(str(fonts / "NotoSans-Bold.ttf"), 30)
    for text, font, y, color in (("GR:FS", bold, 360, (235, 242, 248)),
                                 ("PS5 PORT", small, 448, ACCENT)):
        w = draw.textlength(text, font=font)
        draw.text(((512 - w) / 2, y), text, font=font, fill=color)
    icon.convert("RGB").save(out / "icon0.png")
    for name in ("background.png", "logo.png", "splash.png", "icon0.png"):
        print(out / name)


if __name__ == "__main__":
    main()
