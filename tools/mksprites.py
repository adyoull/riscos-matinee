#!/usr/bin/env python3
"""Write !PlexRO's !Sprites: '!plexro' (34x34) and 'sm!plexro' (17x17),
32 bpp (TBGR) new-format sprites with a 1 bpp mask, as riscos-ffmpeg's
mksprites.py makes Reel's. The picture is our own, drawn from shapes: a
dark screen with three posters on a shelf, and a play button in front.

Usage: mksprites.py OUT_FILE   (write it as !Sprites,ff9)
"""
import struct
import sys

SCREEN = (34, 46, 64)
EDGE = (20, 26, 36)
POSTERS = [(70, 150, 200), (230, 196, 110), (120, 190, 140)]
PLAY_BG = (210, 55, 65)
WHITE = (250, 250, 250)


def draw(size):
    """Rows of (r, g, b) or None (transparent) for a size x size icon."""
    s = size / 34.0
    rows = []
    for y in range(size):
        row = []
        for x in range(size):
            fx, fy = (x + 0.5) / s, (y + 0.5) / s      # in 34-unit space, y down
            px = None
            # the screen: a rounded rectangle 1..33 x 3..29
            if 1 <= fx <= 33 and 3 <= fy <= 29:
                corner = min(fx - 1, 33 - fx, fy - 3, 29 - fy)
                px = EDGE if corner < 1.4 else SCREEN
                if corner < 0.5:
                    px = None if corner < 0.15 else EDGE
                # three posters, 2:3, on a row
                for i, colour in enumerate(POSTERS):
                    x0 = 4.5 + i * 8.5
                    if x0 <= fx <= x0 + 7 and 7 <= fy <= 17.5:
                        px = colour
                        if fy > 15.5:                   # the title strip
                            px = tuple(c // 2 for c in colour)
                # a shelf line under them
                if 19 <= fy <= 20 and 4 <= fx <= 30:
                    px = (60, 76, 100)
            # the stand
            if 29 <= fy <= 32 and 13 <= fx <= 21:
                px = EDGE
            # a round play button, bottom right, over everything
            cx, cy, r = 24.5, 23.5, 7.5
            d2 = (fx - cx) ** 2 + (fy - cy) ** 2
            if d2 <= (r + 0.8) ** 2:
                px = EDGE
            if d2 <= r ** 2:
                px = PLAY_BG
                tx, ty = fx - (cx - 2.8), fy - cy
                if 0 <= tx <= 7 and abs(ty) <= (7 - tx) * 0.62:
                    px = WHITE
            row.append(px)
        rows.append(row)
    return rows


def sprite(name, size):
    rows = draw(size)
    w = h = size
    img = bytearray()
    mask = bytearray()
    mask_row_words = (w + 31) // 32
    for y in range(h):                  # sprites are stored top row first
        mbits = [0] * (mask_row_words * 32)
        for x in range(w):
            px = rows[y][x]
            if px:
                r, g, b = px
                img += bytes((r, g, b, 0))      # TBGR: 0x00BBGGRR
                mbits[x] = 1
            else:
                img += b"\0\0\0\0"
        for wd in range(mask_row_words):
            v = 0
            for bit in range(32):
                if mbits[wd * 32 + bit]:
                    v |= 1 << bit
            mask += struct.pack("<I", v)
    mode = (6 << 27) | (90 << 14) | (90 << 1) | 1
    img_off = 44
    mask_off = img_off + len(img)
    total = mask_off + len(mask)
    hdr = struct.pack("<I12siiiiiii", total, name.encode().ljust(12, b"\0"),
                      w - 1, h - 1, 0, 31, img_off, mask_off, mode)
    return hdr + img + mask


def main():
    sprites = [sprite("!plexro", 34), sprite("sm!plexro", 17)]
    body = b"".join(sprites)
    # the file is a sprite area without its first word: count, first, free
    area = struct.pack("<iii", len(sprites), 16, 16 + len(body)) + body
    open(sys.argv[1], "wb").write(area)


main()
