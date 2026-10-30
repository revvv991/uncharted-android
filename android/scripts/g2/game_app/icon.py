#!/usr/bin/env python3
"""The app's icon (issue #4): an adaptive icon drawn here, no game artwork. A compass rose on a dark
ground, the north needle in the launcher's accent blue. Writes res/mipmap-xxxhdpi/ic_launcher_
{background,foreground}.png (432x432, the adaptive icon's full size; the visible circle is the
middle 66%) and res/mipmap-xxxhdpi/ic_launcher.png (192x192, the legacy icon). Run from anywhere:
    python3 icon.py [res dir]
Needs Pillow."""
import math
import os
import sys

from PIL import Image, ImageDraw

RES = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), 'res')
SIZE = 432          # adaptive layers: 108 dp at xxxhdpi
SAFE = 0.66         # the part never masked
GROUND = (0x16, 0x1B, 0x24)
INK = (0xEE, 0xF1, 0xF5)
ACCENT = (0x3D, 0x8B, 0xFF)
DIM = (0x8A, 0x93, 0xA3)
SS = 4              # supersampling


def rose(draw, cx, cy, r, scale):
    """A compass rose: a thin ring, four long points, four short ones, the north point in blue."""
    ring = max(1, int(3 * scale))
    draw.ellipse((cx - r, cy - r, cx + r, cy + r), outline=DIM, width=ring)
    tick = r * 0.08
    for i in range(8):
        a = i * math.pi / 4
        x0, y0 = cx + (r - tick) * math.sin(a), cy - (r - tick) * math.cos(a)
        x1, y1 = cx + r * math.sin(a), cy - r * math.cos(a)
        draw.line((x0, y0, x1, y1), fill=DIM, width=ring)
    long_r, short_r, half = r * 0.84, r * 0.46, r * 0.11
    for i in range(4):
        a = i * math.pi / 2 + math.pi / 4
        tip = (cx + short_r * math.sin(a), cy - short_r * math.cos(a))
        l = (cx + half * 0.7 * math.sin(a - math.pi / 2), cy - half * 0.7 * math.cos(a - math.pi / 2))
        rr = (cx + half * 0.7 * math.sin(a + math.pi / 2), cy - half * 0.7 * math.cos(a + math.pi / 2))
        draw.polygon([tip, l, rr], fill=DIM)
    for i in range(4):
        a = i * math.pi / 2
        tip = (cx + long_r * math.sin(a), cy - long_r * math.cos(a))
        l = (cx + half * math.sin(a - math.pi / 2), cy - half * math.cos(a - math.pi / 2))
        rr = (cx + half * math.sin(a + math.pi / 2), cy - half * math.cos(a + math.pi / 2))
        colour = ACCENT if i == 0 else INK
        draw.polygon([tip, l, (cx, cy)], fill=colour)
        shade = tuple(max(0, c - 70) for c in colour)
        draw.polygon([tip, rr, (cx, cy)], fill=shade)
    hub = r * 0.07
    draw.ellipse((cx - hub, cy - hub, cx + hub, cy + hub), fill=GROUND, outline=INK, width=ring)


def layers(size):
    s = size * SS
    background = Image.new('RGBA', (s, s), GROUND + (255,))
    foreground = Image.new('RGBA', (s, s), (0, 0, 0, 0))
    rose(ImageDraw.Draw(foreground), s / 2, s / 2, s * SAFE / 2 * 0.86, SS)
    return (background.resize((size, size), Image.LANCZOS),
            foreground.resize((size, size), Image.LANCZOS))


def main():
    out = os.path.join(RES, 'mipmap-xxxhdpi')
    os.makedirs(out, exist_ok=True)
    background, foreground = layers(SIZE)
    background.save(os.path.join(out, 'ic_launcher_background.png'))
    foreground.save(os.path.join(out, 'ic_launcher_foreground.png'))
    # The legacy icon (launchers without adaptive icons): the safe part on a rounded square.
    legacy = 192
    s = legacy * SS
    img = Image.new('RGBA', (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.rounded_rectangle((0, 0, s - 1, s - 1), radius=s // 5, fill=GROUND + (255,))
    rose(d, s / 2, s / 2, s * 0.40, SS)
    img.resize((legacy, legacy), Image.LANCZOS).save(os.path.join(out, 'ic_launcher.png'))
    print('icon:', out)


if __name__ == '__main__':
    main()
