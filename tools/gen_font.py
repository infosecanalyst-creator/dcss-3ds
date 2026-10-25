#!/usr/bin/env python3
"""Generate font5x10.h: a 5x10 1-bpp bitmap font for the DCSS 3DS console.

Renders glyphs from a monospace TTF, thresholds them into a 5x10 cell and
emits a C header with one uint16_t row-bitmap per glyph row (bit 4 = left
pixel).  Glyph lookup is a small sorted table of (codepoint -> index).

Usage:  gen_font.py [DejaVuSansMono.ttf] [out.h] [--preview out.png]
"""
import sys
from PIL import Image, ImageFont, ImageDraw

import os
CELL_W, CELL_H = int(os.environ.get("CELL_W", 5)), int(os.environ.get("CELL_H", 10))
FONT_NAME = os.environ.get("FONT_NAME", "font")
USE_HAND = CELL_W == 5 and CELL_H == 10

# Codepoints to include: ASCII, Latin-1 bits crawl uses, box drawing, blocks,
# and the symbols crawl's default unicode console glyph set draws.
RANGES = [(0x20, 0x7E), (0xA0, 0xFF), (0x2500, 0x257F), (0x2580, 0x259F),
          (0x2190, 0x2199), (0x2600, 0x2667)]
EXTRA = [0x2219, 0x2229, 0x221E, 0x2248, 0x2261, 0x2264, 0x2265, 0x2302,
         0x25CA, 0x2320, 0x2321, 0x03A9, 0x03A6, 0x03B1, 0x03B2, 0x03B3,
         0x03B4, 0x03B5, 0x03C0, 0x03C3, 0x03C4, 0x0394, 0x0398, 0x039E,
         0x2122, 0x2026, 0x2022, 0x2018, 0x2019, 0x201C, 0x201D, 0x2013,
         0x2014, 0x20AC, 0x2591, 0x2592, 0x2593, 0x2588, 0x263A, 0x263B,
         0x2660, 0x2663, 0x2665, 0x2666, 0x266A, 0x266B, 0x25BA, 0x25C4,
         0x25B2, 0x25BC, 0x2195, 0x2194, 0x21A8, 0x203C, 0x00B6, 0x00A7]


def codepoints():
    cps = set(EXTRA)
    for a, b in RANGES:
        cps.update(range(a, b + 1))
    return sorted(cps)


def render(font_path, size, cp):
    """Render one codepoint into a CELL_W x CELL_H boolean grid."""
    font = ImageFont.truetype(font_path, size)
    big = Image.new("L", (CELL_W * 8, CELL_H * 8), 0)
    d = ImageDraw.Draw(big)
    # draw at 8x scale then box-filter down: gives a usable threshold
    f8 = ImageFont.truetype(font_path, size * 8)
    asc, desc = f8.getmetrics()
    ch = chr(cp)
    try:
        bbox = f8.getbbox(ch)
    except Exception:
        bbox = None
    if not bbox or bbox[2] - bbox[0] == 0:
        return None
    # horizontally centre glyph advance in cell
    adv = f8.getlength(ch)
    x0 = (CELL_W * 8 - adv) / 2
    d.text((x0, 0), ch, fill=255, font=f8)
    small = big.resize((CELL_W, CELL_H), Image.BOX)
    px = small.load()
    grid = [[px[x, y] > 72 for x in range(CELL_W)] for y in range(CELL_H)]
    return grid


# Hand-tuned bitmaps for glyphs where TTF downsampling is poor.  Rows are
# strings of '#'/'.', 5 wide, 10 tall.
HAND = {
    0x2500: ["....."] * 4 + ["#####"] + ["....."] * 5,                 # ─
    0x2502: ["..#.."] * 10,                                             # │
    0x250C: ["....."] * 4 + ["..###"] + ["..#.."] * 5,                  # ┌
    0x2510: ["....."] * 4 + ["###.."] + ["..#.."] * 5,                  # ┐
    0x2514: ["..#.."] * 4 + ["..###"] + ["....."] * 5,                  # └
    0x2518: ["..#.."] * 4 + ["###.."] + ["....."] * 5,                  # ┘
    0x251C: ["..#.."] * 4 + ["..###"] + ["..#.."] * 5,                  # ├
    0x2524: ["..#.."] * 4 + ["###.."] + ["..#.."] * 5,                  # ┤
    0x252C: ["....."] * 4 + ["#####"] + ["..#.."] * 5,                  # ┬
    0x2534: ["..#.."] * 4 + ["#####"] + ["....."] * 5,                  # ┴
    0x253C: ["..#.."] * 4 + ["#####"] + ["..#.."] * 5,                  # ┼
    0x2550: ["....."] * 3 + ["#####", ".....", "#####"] + ["....."] * 4,  # ═
    0x2551: [".#.#."] * 10,                                             # ║
    0x2588: ["#####"] * 10,                                             # █
    0x2591: ["#.#.#", ".....", ".#.#.", "....."] * 2 + ["#.#.#", "....."],
    0x2592: ["#.#.#", ".#.#."] * 5,
    0x2593: ["#####", ".#.#.", "#####", "#.#.#"] * 2 + ["#####", ".#.#."],
    0x00B7: ["....."] * 4 + ["..#.."] + ["....."] * 5,                  # ·
    0x2219: ["....."] * 4 + [".##..", ".##.."] + ["....."] * 4,         # ∙
    0x2022: ["....."] * 3 + [".###.", ".###.", ".###."] + ["....."] * 4, # •
    0x2302: [".....", "..#..", ".#.#.", "#...#", "#...#", "#...#", "#...#", "#####", ".....", "....."],  # ⌂
    0x2261: [".....", ".....", "#####", ".....", "#####", ".....", "#####", ".....", ".....", "....."],  # ≡
    0x221E: [".....", ".....", ".....", "##.##", "#.#.#", "#.#.#", "##.##", ".....", ".....", "....."],  # ∞
    0x2229: [".....", ".....", ".###.", "#...#", "#...#", "#...#", "#...#", "#...#", ".....", "....."],  # ∩
    0x2248: [".....", ".....", ".....", ".#..#", "#.##.", ".....", ".#..#", "#.##.", ".....", "....."],  # ≈
    0x25CA: [".....", "..#..", ".#.#.", "#...#", "#...#", ".#.#.", "..#..", ".....", ".....", "....."],  # ◊
    0x2660: [".....", "..#..", ".###.", "#####", "#####", ".###.", "..#..", ".###.", ".....", "....."],  # ♠
    0x2663: [".....", ".###.", ".###.", "#####", "#####", "#####", "..#..", ".###.", ".....", "....."],  # ♣
    0x2665: [".....", ".#.#.", "#####", "#####", "#####", ".###.", "..#..", ".....", ".....", "....."],  # ♥
    0x2666: [".....", "..#..", ".###.", "#####", ".###.", "..#..", ".....", ".....", ".....", "....."],  # ♦
    0x263A: [".....", ".###.", "#...#", "##.##", "#...#", "##.##", "#.#.#", ".###.", ".....", "....."],  # ☺
    0x2191: [".....", "..#..", ".###.", "#.#.#", "..#..", "..#..", "..#..", "..#..", ".....", "....."],  # ↑
    0x2193: [".....", "..#..", "..#..", "..#..", "..#..", "#.#.#", ".###.", "..#..", ".....", "....."],  # ↓
    0x2190: [".....", ".....", "..#..", ".#...", "#####", ".#...", "..#..", ".....", ".....", "....."],  # ←
    0x2192: [".....", ".....", "..#..", "...#.", "#####", "...#.", "..#..", ".....", ".....", "....."],  # →
}


def main():
    font_path = sys.argv[1] if len(sys.argv) > 1 else \
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf"
    out = sys.argv[2] if len(sys.argv) > 2 else "font5x10.h"
    preview = None
    if "--preview" in sys.argv:
        preview = sys.argv[sys.argv.index("--preview") + 1]

    glyphs = []  # (cp, rows)
    for cp in codepoints():
        if USE_HAND and cp in HAND:
            grid = [[c == '#' for c in row] for row in HAND[cp]]
        else:
            grid = render(font_path, 9 if CELL_H == 10 else 7, cp)
            if grid is None:
                if cp == 0x20:
                    grid = [[False] * CELL_W for _ in range(CELL_H)]
                else:
                    continue
        rows = [sum((1 << (CELL_W - 1 - x)) for x in range(CELL_W) if grid[y][x])
                for y in range(CELL_H)]
        if cp != 0x20 and not any(rows):
            continue
        glyphs.append((cp, rows))

    with open(out, "w") as f:
        f.write("// Generated by tools/gen_font.py -- do not edit.\n")
        f.write("#pragma once\n#include <stdint.h>\n\n")
        N = FONT_NAME.upper()
        f.write("#define %s_W %d\n#define %s_H %d\n" % (N, CELL_W, N, CELL_H))
        f.write("#define %s_NGLYPHS %d\n\n" % (N, len(glyphs)))
        f.write("static const uint32_t %s_cp[%s_NGLYPHS] = {\n" % (FONT_NAME, N))
        for i, (cp, _) in enumerate(glyphs):
            f.write("0x%04X," % cp + ("\n" if i % 10 == 9 else " "))
        f.write("\n};\n\n")
        f.write("static const uint8_t %s_bits[%s_NGLYPHS][%s_H] = {\n" % (FONT_NAME, N, N))
        for cp, rows in glyphs:
            f.write("{" + ",".join("0x%02X" % r for r in rows) + "}, // U+%04X\n" % cp)
        f.write("};\n")

    if preview:
        cols = 32
        nrows = (len(glyphs) + cols - 1) // cols
        img = Image.new("RGB", (cols * (CELL_W + 1) * 4, nrows * (CELL_H + 1) * 4), (0, 0, 0))
        px = img.load()
        for i, (cp, rows) in enumerate(glyphs):
            gx, gy = (i % cols) * (CELL_W + 1), (i // cols) * (CELL_H + 1)
            for y, r in enumerate(rows):
                for x in range(CELL_W):
                    if r & (1 << (CELL_W - 1 - x)):
                        for dy in range(4):
                            for dx in range(4):
                                px[(gx + x) * 4 + dx, (gy + y) * 4 + dy] = (200, 200, 200)
        img.save(preview)
    print("wrote %s with %d glyphs" % (out, len(glyphs)))


if __name__ == "__main__":
    main()
