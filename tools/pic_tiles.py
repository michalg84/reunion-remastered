#!/usr/bin/env python3
"""Cuts building sprites out of a planets/EPUL<set>.PIC tile sheet (16x16 tiles, 20 per row).

usage: pic_tiles.py SHEET.PIC OUT.png ROWS_OF_TILES
  ROWS_OF_TILES: tile numbers, rows separated by '/', e.g. 228,229/230,231 (Ion cannon)

Writes OUT.png (indexed, the sheet's palette) and OUT_x8.png (an enlargement to look at).
"""
import sys

from PIL import Image

from pic_to_png import decode

TILE, PER_ROW = 16, 20


def main(argv):
    if len(argv) != 4:
        print(__doc__)
        return 1
    w, h, pixels, palette = decode(argv[1])
    sheet = Image.frombytes("P", (w, h), pixels)
    sheet.putpalette(palette)
    rows = [[int(t) for t in r.split(",")] for r in argv[3].split("/")]
    out = Image.new("P", (max(map(len, rows)) * TILE, len(rows) * TILE))
    out.putpalette(palette)
    for ry, row in enumerate(rows):
        for rx, t in enumerate(row):
            x, y = (t % PER_ROW) * TILE, (t // PER_ROW) * TILE
            out.paste(sheet.crop((x, y, x + TILE, y + TILE)), (rx * TILE, ry * TILE))
    out.save(argv[2])
    stem = argv[2].rsplit(".", 1)[0]
    out.convert("RGB").resize((out.width * 8, out.height * 8), Image.NEAREST).save(stem + "_x8.png")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
