# SPDX-License-Identifier: GPL-3.0-or-later
# tools/make-icons.py OUT_DIR : the Pylon mark (assets/icons/logo-pylon.svg's geometry) as icon.png (80 px) and
# largeIcon.png (130 px), the webOS launcher icons, icon160.png and icon320.png for the Homebrew Channel
# listing (app/packaging), plus pylon-512.png. Needs Pillow. The SVG is drawn by
# the app (nanosvg: no clip paths, so the bands are computed shapes); keep both in step.
import sys
from PIL import Image, ImageDraw
S = 16  # supersampling: drawn at 120*S, then scaled down
def px(v): return v * S
def mark(size):
    W = 120 * S
    grad = Image.new('RGBA', (W, W))
    g = ImageDraw.Draw(grad)
    a, b = (0xFF, 0xA6, 0x2B), (0xFF, 0x7A, 0x00)
    for i in range(2 * W):  # diagonal gradient, top left to bottom right
        t = i / (2 * W - 1)
        c = tuple(round(a[k] + (b[k] - a[k]) * t) for k in range(3)) + (255,)
        g.line([(i, 0), (0, i)], fill=c, width=2)
    mask = Image.new('L', (W, W), 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, W - 1, W - 1], radius=px(26), fill=255)
    img = Image.new('RGBA', (W, W), (0, 0, 0, 0))
    img.paste(grad, (0, 0), mask)
    d = ImageDraw.Draw(img)
    tri = [(px(42), px(31)), (px(88), px(56)), (px(42), px(81))]
    d.polygon(tri, fill=(255, 255, 255, 255))
    d.line(tri + [tri[0], tri[1]], fill=(255, 255, 255, 255), width=px(10), joint='curve')
    for x, y in tri:  # round ends at the corners
        r = px(5)
        d.ellipse([x - r, y - r, x + r, y + r], fill=(255, 255, 255, 255))
    deep = (0xE8, 0x6A, 0x00, 255)
    d.polygon([(px(37), px(43)), (px(74.6), px(43)), (px(87.4), px(50)), (px(37), px(50))], fill=deep)
    d.polygon([(px(37), px(62)), (px(87.4), px(62)), (px(74.6), px(69)), (px(37), px(69))], fill=deep)
    return img.resize((size, size), Image.LANCZOS)
out = sys.argv[1]
mark(80).save(out + '/icon.png')
mark(130).save(out + '/largeIcon.png')
mark(160).save(out + '/icon160.png')  # Homebrew Channel listing card
mark(320).save(out + '/icon320.png')  # its detail page
mark(512).save(out + '/pylon-512.png')
