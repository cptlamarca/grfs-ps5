#!/usr/bin/env python3
"""Make the DualSense texture patches (assets/ps5/textures/<hash>.patch) from
the game's own texture data and the redrawn one.

usage: make_texture_patches.py ORIGINAL_DIR REPLACEMENT_DIR OUT_DIR

ORIGINAL_DIR holds <hash>.bin dumps of the game's data (--texture_dump_path),
REPLACEMENT_DIR the same textures redrawn (<hash>.bin, xtex.py encode). Only
the DXT5 blocks whose pixels differ go into a patch: the DualSense art, not
the game's. The runtime (texture_replacement_path) writes them over the
player's own data when the original is loaded, matched by its hash.

Patch: "RXTP", then little-endian u32 version (1), guest data size, block
size and block count, the blocks' byte offsets (u32 each) and the blocks.
"""
import struct
import sys
from pathlib import Path

import numpy as np

BLOCK = 16
# Largest channel difference still counted as the same pixel (re-encoding).
TOLERANCE = 24


def decode_dxt5(blocks):
    """(N, 16) guest blocks (8in16 big-endian) -> (N, 16, 4) RGBA."""
    b = blocks.reshape(-1, 8, 2)[:, :, ::-1].reshape(-1, 16).astype(np.int32)
    n = len(b)
    a0, a1 = b[:, 0], b[:, 1]
    alpha_bits = sum(b[:, 2 + i].astype(np.int64) << (8 * i) for i in range(6))
    alpha_index = (alpha_bits[:, None] >> (3 * np.arange(16))) & 7
    alphas = np.empty((n, 8), np.int32)
    alphas[:, 0], alphas[:, 1] = a0, a1
    big = a0 > a1
    for i in range(1, 7):
        alphas[:, i + 1] = np.where(big, ((7 - i) * a0 + i * a1) // 7, 0)
    for i in range(1, 5):
        alphas[:, i + 1] = np.where(big, alphas[:, i + 1], ((5 - i) * a0 + i * a1) // 5)
    alphas[:, 6] = np.where(big, alphas[:, 6], 0)
    alphas[:, 7] = np.where(big, alphas[:, 7], 255)

    def rgb(c):
        return np.stack([((c >> 11) & 31) * 255 // 31, ((c >> 5) & 63) * 255 // 63,
                         (c & 31) * 255 // 31], axis=1)

    c0, c1 = rgb(b[:, 8] | b[:, 9] << 8), rgb(b[:, 10] | b[:, 11] << 8)
    colors = np.stack([c0, c1, (2 * c0 + c1) // 3, (c0 + 2 * c1) // 3], axis=1)
    color_bits = b[:, 12] | b[:, 13] << 8 | b[:, 14] << 16 | b[:, 15].astype(np.int64) << 24
    color_index = (color_bits[:, None] >> (2 * np.arange(16))) & 3
    out = np.empty((n, 16, 4), np.int32)
    out[:, :, :3] = np.take_along_axis(colors, color_index[:, :, None], axis=1)
    out[:, :, 3] = np.take_along_axis(alphas, alpha_index, axis=1)
    return out


def patch(original, replacement):
    a = np.frombuffer(original, np.uint8).reshape(-1, BLOCK)
    b = np.frombuffer(replacement, np.uint8).reshape(-1, BLOCK)
    differs = np.abs(decode_dxt5(a) - decode_dxt5(b)).max(axis=(1, 2)) > TOLERANCE
    index = np.nonzero(differs)[0]
    return (b"RXTP" + struct.pack("<4I", 1, len(original), BLOCK, len(index)) +
            (index.astype("<u4") * BLOCK).tobytes() + b[index].tobytes()), len(index), len(a)


def main():
    if len(sys.argv) != 4:
        raise SystemExit(__doc__)
    originals, replacements, out = (Path(p) for p in sys.argv[1:])
    out.mkdir(parents=True, exist_ok=True)
    for path in sorted(replacements.glob("*.bin")):
        original = originals / path.name
        if not original.exists():
            print(f"{path.stem}: no original, skipped")
            continue
        data, count, total = patch(original.read_bytes(), path.read_bytes())
        (out / f"{path.stem}.patch").write_bytes(data)
        print(f"{path.stem}: {count} of {total} blocks")


if __name__ == "__main__":
    main()
