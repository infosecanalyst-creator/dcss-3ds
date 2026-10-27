#!/usr/bin/env python3
"""DCSS tile sheets (rltiles/*.png + tiledef-*.cc) -> one 3DS tile atlas.

Usage: gen_atlas.py <rltiles dir> <out.bin> [scale_px=16]
Every tile in crawl's global tile index space (floor, wall, feat, main,
player, gui, icons -- in that order) is cut from its sheet at its logical
size (normally 32x32, placed at offset_x/offset_y), box-filtered down to
scale_px/32 with premultiplied alpha, and stored as RGBA8.

File layout (little endian):
  'T3DS' u32 version=1 u32 count u32 base_px
  count * { u16 w, u16 h, u32 offset }   (offset into pixel blob, in pixels)
  pixel blob: RGBA8888 (R first), premultiplied alpha
"""
import re, struct, sys
import numpy as np
from PIL import Image

SHEETS = ["floor", "wall", "feat", "main", "player", "gui", "icons"]
rl, out = sys.argv[1], sys.argv[2]
S = int(sys.argv[3]) if len(sys.argv) > 3 else 16
pat = re.compile(r"tile_info\((\-?\d+), (\-?\d+), (\-?\d+), (\-?\d+), (\d+), (\d+), (\d+), (\d+)\)")
entries = []
for sh in SHEETS:
    src = open("%s/tiledef-%s.cc" % (rl, sh)).read()
    a = src.index("_tile_%s_info[" % sh)
    b = src.index("};", a)
    infos = [tuple(map(int, m.groups())) for m in pat.finditer(src[a:b])]
    sheet = np.asarray(Image.open("%s/%s.png" % (rl, sh)).convert("RGBA")).astype(np.float32) / 255.0
    for (w, h, ox, oy, sx, sy, ex, ey) in infos:
        canvas = np.zeros((max(h, 1), max(w, 1), 4), np.float32)
        piece = sheet[sy:ey, sx:ex]
        ph, pw = piece.shape[:2]
        x0, y0 = max(ox, 0), max(oy, 0)
        ph = min(ph, canvas.shape[0] - y0); pw = min(pw, canvas.shape[1] - x0)
        if ph > 0 and pw > 0:
            canvas[y0:y0 + ph, x0:x0 + pw] = piece[:ph, :pw]
        # premultiply then box-filter to S/32 of the logical size
        canvas[..., :3] *= canvas[..., 3:4]
        f = 32 // S if 32 % S == 0 else None
        tw, th = max(1, w * S // 32), max(1, h * S // 32)
        if f and canvas.shape[0] % f == 0 and canvas.shape[1] % f == 0:
            small = canvas.reshape(th, f, tw, f, 4).mean(axis=(1, 3))
        else:
            im = Image.fromarray((canvas * 255).astype(np.uint8), "RGBA").resize((tw, th), Image.BOX)
            small = np.asarray(im).astype(np.float32) / 255.0
        entries.append((tw, th, (np.clip(small, 0, 1) * 255 + 0.5).astype(np.uint8)))
    print("%-7s %5d tiles" % (sh, len(infos)))
with open(out, "wb") as f:
    f.write(b"T3DS" + struct.pack("<III", 1, len(entries), S))
    off = 0
    for (w, h, px) in entries:
        f.write(struct.pack("<HHI", w, h, off)); off += w * h
    for (w, h, px) in entries:
        f.write(px.tobytes())
print("%s: %d tiles, %.1f MB" % (out, len(entries), off * 4 / 1e6))
