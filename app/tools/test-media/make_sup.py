# SPDX-License-Identifier: GPL-3.0-or-later
# make_sup.py OUT.sup : a PGS stream with labelled cues ("PGS 2.0 s" ...) every 2 s for 40 s.
import struct, sys
from PIL import Image, ImageDraw, ImageFont

W, H = 1920, 1080
font = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 64)
PAL = [(0, 128, 128, 0), (235, 128, 128, 255), (16, 128, 128, 255), (126, 128, 128, 255)]  # Y Cr Cb A

def segment(pts, typ, body):
    return b"PG" + struct.pack(">II", pts, pts) + bytes([typ]) + struct.pack(">H", len(body)) + body

def rle(rows):
    out = bytearray()
    for row in rows:
        x = 0
        while x < len(row):
            c = row[x]; n = 1
            while x + n < len(row) and row[x + n] == c and n < 16383: n += 1
            if c != 0 and n <= 2:
                out += bytes([c]) * n
            elif n < 64:
                out += bytes([0, n]) if c == 0 else bytes([0, 0x80 | n, c])
            else:
                out += bytes([0, 0x40 | (n >> 8), n & 0xFF]) if c == 0 else bytes([0, 0xC0 | (n >> 8), n & 0xFF, c])
            x += n
        out += b"\0\0"
    return bytes(out)

def image(text):
    img = Image.new("RGBA", (900, 110), (0, 0, 0, 0))
    ImageDraw.Draw(img).text((10, 10), text, font=font, fill=(255, 255, 255, 255), stroke_width=4, stroke_fill=(0, 0, 0, 255))
    img = img.crop(img.getbbox())
    rows = []
    for y in range(img.height):
        row = []
        for x in range(img.width):
            r, g, b, a = img.getpixel((x, y))
            row.append(0 if a < 64 else 1 if r > 180 else 2 if r < 80 else 3)
        rows.append(row)
    return img.width, img.height, rows

out = bytearray()
for i, t in enumerate(range(2, 40, 2)):
    w, h, rows = image("PGS %d.0 s" % t)
    x, y = (W - w) // 2, H - 140 - h
    pts = t * 90000
    out += segment(pts, 0x16, struct.pack(">HHBHBBBB", W, H, 0x10, i * 2, 0x80, 0, 0, 1) + struct.pack(">HBBHH", 0, 0, 0, x, y))
    out += segment(pts, 0x17, struct.pack(">BBHHHH", 1, 0, x, y, w, h))
    out += segment(pts, 0x14, bytes([0, 0]) + b"".join(bytes([k, Y, Cr, Cb, A]) for k, (Y, Cr, Cb, A) in enumerate(PAL)))
    data = rle(rows)
    out += segment(pts, 0x15, struct.pack(">HBB", 0, 0, 0xC0) + struct.pack(">I", len(data) + 4)[1:] + struct.pack(">HH", w, h) + data)
    out += segment(pts, 0x80, b"")
    end = pts + int(1.5 * 90000)  # shown 1.5 s, then cleared
    out += segment(end, 0x16, struct.pack(">HHBHBBBB", W, H, 0x10, i * 2 + 1, 0x00, 0, 0, 0))
    out += segment(end, 0x17, struct.pack(">BBHHHH", 1, 0, x, y, w, h))
    out += segment(end, 0x80, b"")
open(sys.argv[1], "wb").write(out)
