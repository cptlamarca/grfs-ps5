#!/usr/bin/env python3
"""Make the PS5 home screen art: assets/ps5/pic0.dds (background) and
pic1.dds (launch splash), 3840x2160 BC7_UNORM DX10 DDS without mipmaps.

usage: make_ps5_art.py BACKGROUND [LOGO] [OUT_DIR]

BACKGROUND is any image (cropped to 16:9 and scaled); LOGO, a PNG with
transparency, goes on the splash's left. The BC7 encoding is plain (mode 6,
one subset, opaque, box endpoints): good enough for these pictures, with no
encoder to install.
"""
import struct
import sys
from pathlib import Path

import numpy as np
from PIL import Image

WIDTH, HEIGHT = 3840, 2160
# Mode 6's 4-bit index weights.
WEIGHTS = np.array([0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64], np.int32)


def bc7_mode6(rgb):
    """(H, W, 3) uint8 -> BC7 blocks, row by row, 16 bytes each."""
    h, w, _ = rgb.shape
    blocks = (rgb.reshape(h // 4, 4, w // 4, 4, 3).transpose(0, 2, 1, 3, 4)
              .reshape(-1, 16, 3).astype(np.int32))
    out = np.empty((len(blocks), 16), np.uint8)
    for start in range(0, len(blocks), 32768):
        out[start:start + 32768] = encode(blocks[start:start + 32768])
    return out.tobytes()


def encode(px):
    n = len(px)
    # Endpoints: the block's box corners, as 7 bits and a p-bit of 1.
    lo, hi = px.min(axis=1), px.max(axis=1)
    q0 = np.clip((lo - 1 + 1) // 2, 0, 127)
    q1 = np.clip((hi - 1 + 1) // 2, 0, 127)
    e0, e1 = q0 * 2 + 1, q1 * 2 + 1
    palette = ((64 - WEIGHTS)[None, :, None] * e0[:, None, :] +
               WEIGHTS[None, :, None] * e1[:, None, :] + 32) >> 6
    distance = ((px[:, :, None, :] - palette[:, None, :, :]) ** 2).sum(axis=3)
    index = distance.argmin(axis=2)
    # The first pixel's index must have its top bit clear: swap otherwise.
    swap = index[:, 0] >= 8
    q0[swap], q1[swap] = q1[swap].copy(), q0[swap].copy()
    index[swap] = 15 - index[swap]

    bits = np.zeros((n, 128), np.uint8)
    pos = 0

    def put(values, count):
        nonlocal pos
        for b in range(count):
            bits[:, pos + b] = (values >> b) & 1
        pos += count

    put(np.full(n, 1 << 6), 7)  # mode 6
    for c in range(3):
        put(q0[:, c], 7)
        put(q1[:, c], 7)
    put(np.full(n, 127), 7)  # alpha: opaque
    put(np.full(n, 127), 7)
    put(np.ones(n, np.int32), 1)  # p-bits
    put(np.ones(n, np.int32), 1)
    put(index[:, 0], 3)
    for i in range(1, 16):
        put(index[:, i], 4)
    return np.packbits(bits, axis=1, bitorder="little")


def dds(data):
    header = bytearray(148)
    header[0:4] = b"DDS "
    struct.pack_into("<7I", header, 4, 124, 0x1 | 0x2 | 0x4 | 0x1000 | 0x80000, HEIGHT, WIDTH,
                     WIDTH * HEIGHT, 0, 1)
    struct.pack_into("<2I4s", header, 76, 32, 0x4, b"DX10")
    struct.pack_into("<I", header, 108, 0x1000)
    # DXGI_FORMAT_BC7_UNORM, a 2D texture, one of it.
    struct.pack_into("<5I", header, 128, 98, 3, 0, 1, 0)
    return bytes(header) + data


def cover(image):
    """Cropped to 16:9 around its middle and scaled to 3840x2160."""
    image = image.convert("RGB")
    ratio = WIDTH / HEIGHT
    w, h = image.size
    if w / h > ratio:
        crop = int(h * ratio)
        image = image.crop(((w - crop) // 2, 0, (w - crop) // 2 + crop, h))
    else:
        crop = int(w / ratio)
        image = image.crop((0, (h - crop) // 2, w, (h - crop) // 2 + crop))
    return image.resize((WIDTH, HEIGHT), Image.LANCZOS)


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    background = cover(Image.open(sys.argv[1]))
    out = Path(sys.argv[3] if len(sys.argv) > 3 else Path(__file__).parent.parent / "assets/ps5")
    splash = background.copy()
    if len(sys.argv) > 2:
        logo = Image.open(sys.argv[2]).convert("RGBA")
        scale = 1500 / logo.width
        logo = logo.resize((1500, int(logo.height * scale)), Image.LANCZOS)
        splash.paste(logo, (220, (HEIGHT - logo.height) // 2), logo)
    for name, image in (("pic0", background), ("pic1", splash)):
        (out / f"{name}.dds").write_bytes(dds(bc7_mode6(np.asarray(image))))
        print(out / f"{name}.dds")


if __name__ == "__main__":
    main()
