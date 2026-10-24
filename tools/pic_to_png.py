#!/usr/bin/env python3
"""Converts Reunion .PIC pictures ("SpidyGfx", src/assets/pic.c) to PNG.

usage: pic_to_png.py OUT_DIR NAME=FILE.PIC [NAME=FILE.PIC ...]

For each picture writes NAME.png (indexed, the picture's own VGA palette, the
exact pixels) and NAME_x4.png (RGB, enlarged 4x with no smoothing, to look at).
"""
import struct
import sys
from pathlib import Path

from PIL import Image

HEADER, PALETTE = 12, 768


def rows(data, width, height):
    """The game's RLE: a byte >= 0xC0 is a run of (b & 0x3F) of the next byte."""
    pos, out, bad = HEADER, bytearray(), 0
    for _ in range(height):
        col = 0
        while col < width:
            b = data[pos]
            pos += 1
            n = 1
            if b >= 0xC0:
                n = b & 0x3F
                b = data[pos]
                pos += 1
            out += bytes([b]) * n
            col += n
        bad += col != width
    return out, bad


def decode(path):
    data = Path(path).read_bytes()
    if data[:8] != b"SpidyGfx":
        raise ValueError(f"{path}: not a PIC")
    width, height = struct.unpack_from("<HH", data, 8)
    pixels, bad = rows(data, width, height)
    if bad:  # INFO26.PIC style: the header is one short
        pixels, bad = rows(data, width + 1, height)
        width += 1
    raw = data[-PALETTE:]
    palette = [(v >> 2) * 255 // 63 for v in raw]  # 6-bit VGA -> 8-bit
    return width, height, bytes(pixels[: width * height]), palette


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 1
    out = Path(argv[1])
    out.mkdir(parents=True, exist_ok=True)
    for spec in argv[2:]:
        name, _, src = spec.partition("=")
        w, h, pixels, palette = decode(src)
        img = Image.frombytes("P", (w, h), pixels)
        img.putpalette(palette)
        img.save(out / f"{name}.png")
        img.convert("RGB").resize((w * 4, h * 4), Image.NEAREST).save(out / f"{name}_x4.png")
        print(f"{name}: {w}x{h} from {src}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
