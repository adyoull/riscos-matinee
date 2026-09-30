#!/usr/bin/env python3
"""Write !PlexRO's sprite files: '!plexro' (the icon bar and Filer icon) and
'sm!plexro' (small), 32 bpp (TBGR) new-format sprites with a 1 bpp mask,
as riscos-ffmpeg's mksprites.py makes Reel's.

  mksprites.py OUT        !Sprites: 34 x 34 and 17 x 17 at 90 dpi
  mksprites.py --hi OUT   !Sprites11: 68 x 68 and 34 x 34 at 180 dpi (RISC OS
                          5 uses these in high-resolution modes)

The picture is our own, drawn from shapes, in the browser's colours: a
dark rounded screen with three posters on it (the middle one in front),
and a blue play button. Each pixel is the average of 6 x 6 samples, so
the edges inside it are smooth; the mask keeps the pixels more than half
covered, and those on the outer edge are blended with the icon bar's grey.
"""
import struct
import sys

SCREEN = (30, 33, 40)
SCREEN_EDGE = (14, 15, 19)
POSTERS = [((70, 110, 190), (40, 60, 120)),      # top and bottom colours
           ((236, 190, 90), (190, 110, 50)),
           ((90, 190, 150), (40, 110, 90))]
SHADOW = (10, 10, 12)
ACCENT = (70, 150, 235)
WHITE = (250, 250, 250)
ICONBAR = (221, 221, 221)
SS = 6


def rounded(fx, fy, x0, y0, x1, y1, r):
    """Inside a rounded rectangle?"""
    if not (x0 <= fx <= x1 and y0 <= fy <= y1):
        return False
    cx = min(max(fx, x0 + r), x1 - r)
    cy = min(max(fy, y0 + r), y1 - r)
    return (fx - cx) ** 2 + (fy - cy) ** 2 <= r * r


def mix(a, b, t):
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3))


def sample(fx, fy):
    """The colour at a point in 34-unit space (y down), or None."""
    px = None
    if rounded(fx, fy, 1, 3.5, 33, 30.5, 5):
        px = SCREEN_EDGE
        if rounded(fx, fy, 2, 4.5, 32, 29.5, 4):
            px = mix(SCREEN, SCREEN_EDGE, max(0.0, (fy - 5) / 40))
    # the stand
    if 13 <= fx <= 21 and 30 <= fy <= 32.5:
        px = SCREEN_EDGE
    if px is None:
        return None
    # three posters: the outer two behind, the middle one in front
    boxes = [(4.5, 8, 11.5, 19, 0), (22.5, 8, 29.5, 19, 2), (11, 6.5, 20, 20.5, 1)]
    for x0, y0, x1, y1, k in boxes:
        if rounded(fx, fy, x0 + 0.8, y0 + 0.8, x1 + 0.8, y1 + 0.8, 1.2):
            px = SHADOW
        if rounded(fx, fy, x0, y0, x1, y1, 1.2):
            top, bottom = POSTERS[k]
            px = mix(top, bottom, (fy - y0) / (y1 - y0))
    # a progress bar under them
    if 5 <= fx <= 20 and 23 <= fy <= 24.5:
        px = ACCENT if fx <= 13 else (70, 76, 90)
    # the play button, bottom right, over everything
    cx, cy, r = 25.5, 24.5, 6.8
    d2 = (fx - cx) ** 2 + (fy - cy) ** 2
    if d2 <= (r + 1.0) ** 2:
        px = SCREEN_EDGE
    if d2 <= r * r:
        px = ACCENT
        tx, ty = fx - (cx - 2.4), fy - cy
        if 0 <= tx <= 6.2 and abs(ty) <= (6.2 - tx) * 0.6:
            px = WHITE
    return px


def draw(size):
    """Rows of (r, g, b) or None for a size x size icon."""
    s = size / 34.0
    rows = []
    for y in range(size):
        row = []
        for x in range(size):
            acc = [0, 0, 0]
            n = 0
            for j in range(SS):
                for i in range(SS):
                    c = sample((x + (i + 0.5) / SS) / s, (y + (j + 0.5) / SS) / s)
                    if c:
                        n += 1
                        for k in range(3):
                            acc[k] += c[k]
            if n * 2 < SS * SS:
                row.append(None)
            else:
                # partly covered: blend what's missing with the icon bar's grey
                miss = SS * SS - n
                row.append(tuple((acc[k] + ICONBAR[k] * miss) // (SS * SS) for k in range(3)))
        rows.append(row)
    return rows


def sprite(name, size, dpi):
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
    mode = (6 << 27) | (dpi << 14) | (dpi << 1) | 1
    img_off = 44
    mask_off = img_off + len(img)
    total = mask_off + len(mask)
    hdr = struct.pack("<I12siiiiiii", total, name.encode().ljust(12, b"\0"),
                      w - 1, h - 1, 0, 31, img_off, mask_off, mode)
    return hdr + img + mask


def main():
    args = sys.argv[1:]
    hi = args and args[0] == "--hi"
    if hi:
        args = args[1:]
        sprites = [sprite("!plexro", 68, 180), sprite("sm!plexro", 34, 180)]
    else:
        sprites = [sprite("!plexro", 34, 90), sprite("sm!plexro", 17, 90)]
    body = b"".join(sprites)
    # the file is a sprite area without its first word: count, first, free
    area = struct.pack("<iii", len(sprites), 16, 16 + len(body)) + body
    open(args[0], "wb").write(area)


if __name__ == "__main__":
    main()
