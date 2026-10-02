#!/usr/bin/env python3
"""Write !Matinee's sprite files: '!matinee' (the icon bar and Filer icon) and
'sm!matinee' (small), 32 bpp (TBGR) new-format sprites with a 1 bpp mask,
as riscos-ffmpeg's mksprites.py makes Reel's.

  mksprites.py OUT        !Sprites: 34 x 34 and 17 x 17 at 90 dpi
  mksprites.py --hi OUT   !Sprites11: 68 x 68 and 34 x 34 at 180 dpi (RISC OS
                          5 uses these in high-resolution modes)

The picture is our own, drawn from shapes: a little theatre for a
matinee, red curtains drawn back and tied with gold from a lit screen
under a scalloped pelmet, in a gold-edged frame, with a play triangle on
the screen. Each pixel is the average of 6 x 6 samples, so
the edges inside it are smooth; the mask keeps the pixels more than half
covered, and those on the outer edge are blended with the icon bar's grey.
"""
import struct
import sys

FRAME = (52, 30, 18)                    # the proscenium: dark wood
GOLD = (226, 178, 72)
GOLD_DARK = (150, 104, 36)
RED = (176, 26, 36)
RED_DARK = (92, 10, 18)
SCREEN_TOP = (255, 246, 214)            # the screen, lit by the projector
SCREEN_FOOT = (236, 206, 150)
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
    t = max(0.0, min(1.0, t))
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3))


def curtain(fx, fy, left):
    """A red curtain, drawn back and tied at about two thirds of the way
    down: its inner edge curves in to the tie and out again below it.
    The folds are bands of light and shade down it."""
    import math
    top, tie, foot = 7.0, 20.0, 29.5
    if not (top <= fy <= foot):
        return None
    # the inner edge's distance from the side, in 34ths
    if fy <= tie:
        t = (fy - top) / (tie - top)
        reach = 9.0 - 5.2 * math.sin(t * math.pi / 2)
    else:
        t = (fy - tie) / (foot - tie)
        reach = 3.8 + 3.2 * t * t
    x = fx - 3.0 if left else 31.0 - fx
    if not (0 <= x <= reach):
        return None
    fold = 0.5 + 0.5 * math.cos((x / max(reach, 1.0)) * math.pi * 2.6)
    c = mix(RED_DARK, RED, 0.35 + 0.65 * fold)
    if x > reach - 0.9:                 # the edge in shadow
        c = mix(c, RED_DARK, 0.6)
    # the gold tie-back
    if abs(fy - tie) <= 0.9 and x <= reach + 0.2:
        c = GOLD if abs(fy - tie) <= 0.5 else GOLD_DARK
    return c


def sample(fx, fy):
    """The colour at a point in 34-unit space (y down), or None: a little
    theatre: red curtains drawn back from a lit screen under a gold-trimmed
    pelmet, in a dark frame, with a play triangle on the screen."""
    import math
    if not rounded(fx, fy, 1, 2, 33, 32, 4):
        return None
    px = FRAME
    if rounded(fx, fy, 1.8, 2.8, 32.2, 31.2, 3.4):
        px = mix(GOLD, GOLD_DARK, (fy - 3) / 28)        # the gold border
    if rounded(fx, fy, 3, 4, 31, 30, 2.4):
        # the screen: lit from above, darker towards the foot
        px = mix(SCREEN_TOP, SCREEN_FOOT, (fy - 8) / 22)
        # the play triangle, soft gold
        tx, ty = fx - 14.0, fy - 18.5
        if 0 <= tx <= 7.4 and abs(ty) <= (7.4 - tx) * 0.62:
            px = mix(GOLD, GOLD_DARK, 0.25)
        # the curtains, over the screen's sides
        for left in (True, False):
            c = curtain(fx, fy, left)
            if c:
                px = c
        # the pelmet across the top: red, scalloped, with a gold fringe
        scallop = 8.4 + 1.3 * abs(math.sin((fx - 3) * math.pi / 4.0))
        if fy <= scallop:
            px = mix(RED, RED_DARK, (fy - 4) / 6)
            if fy >= scallop - 0.8:
                px = GOLD
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
        sprites = [sprite("!matinee", 68, 180), sprite("sm!matinee", 34, 180)]
    else:
        sprites = [sprite("!matinee", 34, 90), sprite("sm!matinee", 17, 90)]
    body = b"".join(sprites)
    # the file is a sprite area without its first word: count, first, free
    area = struct.pack("<iii", len(sprites), 16, 16 + len(body)) + body
    open(args[0], "wb").write(area)


if __name__ == "__main__":
    main()
