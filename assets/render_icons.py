#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Felipe Basurto
"""Renders the whole Lodestar identity from one set of shapes (specs/panel.md, PANEL-7).

    python3 assets/render_icons.py

Only needs Pillow. Writes the mark (a flying wing under the star it steers by), the wordmark, the
lockup, the PS4 tile (icon0.png) and home screen art (pic1.png), the browser extension icons and the
header lockup the native app draws (panel-app/Brand.png).

All art is original and flat. The wordmark is outlined from Michroma, SIL Open Font License 1.1
(third_party/michroma/OFL.txt). No game asset, logo or typeface is used.
"""

import math
import os
import re

from PIL import Image, ImageChops, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

BG = "#0a0b0d"
WHITE = "#f3f4f5"
ORANGE = "#ff6a13"
SS = 4  # supersampling

# The mark on a 48 x 48 grid. Its box is x 1..47, y 0.5..41.
WING = [(24, 19), (47, 36), (45, 39), (33, 35.5), (28.5, 41), (24, 36.5), (19.5, 41), (15, 35.5), (3, 39), (1, 36)]
MARK_X, MARK_Y, MARK_W, MARK_H = 1, 0.5, 46, 40.5


def star(cx=24, cy=9, up=8.5, side=6.5, down=6.5, pull=0.18, steps=12):
    """A four-point star with concave sides: each side bends towards the centre."""
    tips = [(cx, cy - up), (cx + side, cy), (cx, cy + down), (cx - side, cy)]
    points = []
    for i in range(4):
        a, b = tips[i], tips[(i + 1) % 4]
        c = (cx + ((a[0] + b[0]) / 2 - cx) * pull, cy + ((a[1] + b[1]) / 2 - cy) * pull)
        for j in range(steps):
            t = j / steps
            points.append(tuple((1 - t) ** 2 * a[k] + 2 * (1 - t) * t * c[k] + t * t * b[k] for k in (0, 1)))
    return points


STAR = star()
# Under 48 px the star needs more body than the drawing gives it.
STAR_SMALL = star(cy=9.5, up=10, side=9.5, down=8, pull=0.34)


def mark(x, y, width, point=None):
    """Shapes of the mark, (colour, polygon), with its box's left top corner at x, y."""
    k = width / MARK_W

    def at(points):
        return [(x + (px - MARK_X) * k, y + (py - MARK_Y) * k) for px, py in points]

    return [(WHITE, at(WING)), (ORANGE, at(point or STAR))]


def centred(cx, cy, width, point=None):
    return mark(cx - width / 2, cy - width * MARK_H / MARK_W / 2, width, point)


# LODESTAR, outlined: path data in font units, capitals 1536 high on the baseline y = 0.
WORDMARK = (
    'M2780.76 32Q2555.76 32 2410.76 1.5Q2265.76 -29 2184.26 -111Q2102.76 -193 2070.26 -347Q2037.76 -501 2037.76 -'
    '748V-788Q2037.76 -992 2057.76 -1130.5Q2077.76 -1269 2126.76 -1356Q2175.76 -1443 2260.26 -1489Q2344.76 -1535 '
    '2472.76 -1551.5Q2600.76 -1568 2780.76 -1568H3183.76Q3363.76 -1568 3491.76 -1551.5Q3619.76 -1535 3704.26 -148'
    '9Q3788.76 -1443 3837.76 -1356Q3886.76 -1269 3906.76 -1130.5Q3926.76 -992 3926.76 -788V-748Q3926.76 -501 3894'
    '.26 -347Q3861.76 -193 3780.26 -111Q3698.76 -29 3553.76 1.5Q3408.76 32 3183.76 32ZM4496.52 0V-1536H5636.52Q58'
    '61.52 -1536 6006.52 -1509Q6151.52 -1482 6233.02 -1404.5Q6314.52 -1327 6347.02 -1179Q6379.52 -1031 6379.52 -7'
    '88V-748Q6379.52 -513 6347.02 -365.5Q6314.52 -218 6233.02 -138.5Q6151.52 -59 6006.52 -29.5Q5861.52 0 5636.52 '
    '0ZM2780.76 -134H3183.76Q3326.76 -134 3424.76 -144Q3522.76 -154 3584.26 -187Q3645.76 -220 3678.26 -287Q3710.7'
    '6 -354 3722.76 -466Q3734.76 -578 3734.76 -748V-788Q3734.76 -964 3722.76 -1078Q3710.76 -1192 3678.26 -1258Q36'
    '45.76 -1324 3584.26 -1354.5Q3522.76 -1385 3424.76 -1393.5Q3326.76 -1402 3183.76 -1402H2780.76Q2637.76 -1402 '
    '2539.76 -1393.5Q2441.76 -1385 2380.26 -1354.5Q2318.76 -1324 2286.26 -1258Q2253.76 -1192 2241.76 -1078Q2229.7'
    '6 -964 2229.76 -788V-748Q2229.76 -578 2241.76 -466Q2253.76 -354 2286.26 -287Q2318.76 -220 2380.26 -187Q2441.'
    '76 -154 2539.76 -144Q2637.76 -134 2780.76 -134ZM4688.52 -166H5636.52Q5815.52 -166 5924.52 -183Q6033.52 -200 '
    '6090.52 -257Q6147.52 -314 6167.52 -431Q6187.52 -548 6187.52 -748V-788Q6187.52 -954 6175.52 -1062Q6163.52 -11'
    '70 6131.02 -1232.5Q6098.52 -1295 6037.02 -1324.5Q5975.52 -1354 5877.52 -1362Q5779.52 -1370 5636.52 -1370H468'
    '8.52ZM15632.32 -1536H16660.32Q16885.32 -1536 17022.82 -1516Q17160.32 -1496 17231.32 -1447Q17302.32 -1398 173'
    '27.32 -1313Q17352.32 -1228 17352.32 -1098V-1058Q17352.32 -938 17323.82 -851.5Q17295.32 -765 17214.32 -711.5Q'
    '17133.32 -658 16974.32 -636L17308.32 0H17116.32L16789.32 -622Q16769.32 -621 16747.82 -621H15824.32V0H15632.3'
    '2ZM13078.56 0 13974.56 -1536H14230.56L15126.56 0H14902.56L14710.56 -352H13494.56L13302.56 0ZM10618.04 -444Q1'
    '0618.04 -533 10604.04 -584Q10590.04 -635 10541.04 -659.5Q10492.04 -684 10389.04 -691.5Q10286.04 -699 10107.0'
    '4 -702L9551.04 -710Q9345.04 -713 9220.04 -735Q9095.04 -757 9031.04 -804.5Q8967.04 -852 8945.54 -932Q8924.04 '
    '-1012 8924.04 -1132Q8924.04 -1263 8953.54 -1348Q8983.04 -1433 9060.54 -1481Q9138.04 -1529 9280.04 -1548.5Q94'
    '22.04 -1568 9647.04 -1568H10075.04Q10279.04 -1568 10415.04 -1555Q10551.04 -1542 10631.04 -1495Q10711.04 -144'
    '8 10745.54 -1349Q10780.04 -1250 10780.04 -1077H10588.04Q10588.04 -1211 10562.54 -1279.5Q10537.04 -1348 10478'
    '.54 -1373Q10420.04 -1398 10321.04 -1400Q10222.04 -1402 10075.04 -1402H9657.04Q9478.04 -1402 9370.54 -1395Q92'
    '63.04 -1388 9208.04 -1363Q9153.04 -1338 9134.54 -1283Q9116.04 -1228 9116.04 -1132Q9116.04 -1051 9129.04 -100'
    '2Q9142.04 -953 9187.54 -927.5Q9233.04 -902 9328.04 -892.5Q9423.04 -883 9587.04 -881L10183.04 -873Q10393.04 -'
    '870 10518.54 -848.5Q10644.04 -827 10706.54 -779Q10769.04 -731 10789.54 -649.5Q10810.04 -568 10810.04 -444Q10'
    '810.04 -309 10784.54 -217.5Q10759.04 -126 10686.04 -71Q10613.04 -16 10475.04 8Q10337.04 32 10112.04 32H9644.'
    '04Q9439.04 32 9300.04 14Q9161.04 -4 9078.54 -58.5Q8996.04 -113 8960.04 -221Q8924.04 -329 8924.04 -509H9116.0'
    '4Q9116.04 -368 9139.04 -291Q9162.04 -214 9219.54 -180.5Q9277.04 -147 9380.54 -140.5Q9484.04 -134 9644.04 -13'
    '4H10102.04Q10281.04 -134 10385.04 -145.5Q10489.04 -157 10539.04 -189.5Q10589.04 -222 10603.54 -283Q10618.04 '
    '-344 10618.04 -444ZM6945.28 0V-1536H8453.28V-1376H7137.28V-864H8398.28V-704H7137.28V-160H8481.28V0ZM15824.32'
    ' -787H16673.32Q16844.32 -787 16942.82 -798.5Q17041.32 -810 17087.82 -839Q17134.32 -868 17147.32 -921Q17160.3'
    '2 -974 17160.32 -1058V-1098Q17160.32 -1193 17142.32 -1248Q17124.32 -1303 17070.32 -1329Q17016.32 -1355 16909'
    '.32 -1362.5Q16802.32 -1370 16623.32 -1370H15824.32ZM12040.8 0V-1376H11272.8V-1536H13000.8V-1376H12232.8V0ZM1'
    '92 0V-1536H384V-160H1600V0ZM13590.56 -512H14614.56L14102.56 -1408Z'
)
CAP, LEFT, TOP, WIDE, TALL = 1536, 192, -1568, 17160.32, 1600


def contours(d, steps=8):
    """The rings of path data made of M, L, H, V, Q and Z."""
    rings, ring, x, y = [], [], 0.0, 0.0
    for command, rest in re.findall(r"([MLHVQZ])([^MLHVQZ]*)", d):
        n = [float(v) for v in re.findall(r"-?\d+(?:\.\d+)?", rest)]
        if command in "ML":
            for i in range(0, len(n), 2):
                x, y = n[i], n[i + 1]
                ring.append((x, y))
        elif command in "HV":
            for v in n:
                x, y = (v, y) if command == "H" else (x, v)
                ring.append((x, y))
        elif command == "Q":
            for i in range(0, len(n), 4):
                ax, ay, bx, by = n[i:i + 4]
                for j in range(1, steps + 1):
                    t = j / steps
                    ring.append(((1 - t) ** 2 * x + 2 * (1 - t) * t * ax + t * t * bx,
                                 (1 - t) ** 2 * y + 2 * (1 - t) * t * ay + t * t * by))
                x, y = bx, by
        else:
            rings.append(ring)
            ring = []
    return rings


def wordmark_width(cap):
    return WIDE * cap / CAP


def wordmark(x, baseline, cap, colour=WHITE):
    """One shape with all its rings (even-odd), its left edge at x."""
    k = cap / CAP
    return [(colour, [[(x + (px - LEFT) * k, baseline + py * k) for px, py in ring] for ring in contours(WORDMARK)])]


# ---- SVG ----

def path_d(rings):
    return "".join("M" + "L".join(f"{x:.2f} {y:.2f}" for x, y in ring) + "Z" for ring in rings)


def svg(width, height, shapes, background=None):
    out = [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {width:g} {height:g}" width="{width:g}" '
           f'height="{height:g}">']
    if background:
        out.append(f'<rect width="100%" height="100%" fill="{background}"/>')
    for colour, shape in shapes:
        rings = shape if isinstance(shape[0], list) else [shape]
        out.append(f'<path fill="{colour}" fill-rule="evenodd" d="{path_d(rings)}"/>')
    out.append("</svg>\n")
    return "".join(out)


# ---- raster ----

def rgb(colour):
    return tuple(int(colour[i:i + 2], 16) for i in (1, 3, 5))


def paint(image, shapes):
    """Antialiased even-odd fill of the shapes on an RGBA image."""
    big = (image.width * SS, image.height * SS)
    for colour, shape in shapes:
        rings = shape if isinstance(shape[0], list) else [shape]
        mask = Image.new("1", big, 0)
        for ring in rings:
            one = Image.new("1", big, 0)
            ImageDraw.Draw(one).polygon([(x * SS, y * SS) for x, y in ring], fill=1)
            mask = ImageChops.logical_xor(mask, one)
        image.paste(rgb(colour) + (255,), mask=mask.convert("L").resize(image.size, Image.LANCZOS))
    return image


def ground(width, height, x, y, radius, colour="#1b1f25"):
    """The dark ground with one soft light behind the mark."""
    image = Image.new("RGBA", (width, height), rgb(BG) + (255,))
    small = Image.new("L", (192, 192), 0)
    px = small.load()
    for j in range(192):
        for i in range(192):
            dx = ((i + 0.5) / 192 * width - x) / radius
            dy = ((j + 0.5) / 192 * height - y) / radius
            px[i, j] = int(255 * max(0.0, 1 - math.hypot(dx, dy)) ** 1.5)
    image.paste(rgb(colour) + (255,), mask=small.resize(image.size, Image.BICUBIC))
    return image


def tile(size, wordmarked):
    """The square app art."""
    image = ground(size, size, size / 2, size * 0.40, size * 0.75)
    k = size / 512
    if wordmarked:
        paint(image, centred(256 * k, 226 * k, 300 * k))
        cap = 33 * k
        paint(image, wordmark(256 * k - wordmark_width(cap) / 2, 440 * k, cap))
    else:
        paint(image, centred(256 * k, 256 * k, 330 * k))
    return image


def small_icon(size):
    """Extension and favicon sizes: the mark on a dark square."""
    image = Image.new("RGBA", (size, size), rgb(BG) + (255,))
    paint(image, centred(size / 2, size / 2, size * 0.86, STAR_SMALL if size < 48 else STAR))
    return image


def save(image, *parts):
    path = os.path.join(ROOT, *parts)
    image.convert("RGB").save(path, optimize=True)
    print(f"{os.path.relpath(path, ROOT)}: {image.width}x{image.height}")


def write(text, *parts):
    path = os.path.join(ROOT, *parts)
    with open(path, "w", encoding="utf-8") as out:
        out.write(text)
    print(os.path.relpath(path, ROOT))


HEADER = "<!-- SPDX-License-Identifier: GPL-3.0-or-later -->\n<!-- Generated by assets/render_icons.py -->\n"


def main():
    write(HEADER + svg(512, 512, centred(256, 256, 330), BG), "assets", "lodestar.svg")
    width = round(wordmark_width(100), 2)
    write(HEADER + svg(width, round(TALL * 100 / CAP, 2), wordmark(0, -TOP * 100 / CAP, 100)),
          "assets", "lodestar-wordmark.svg")
    mark_width = 120 * MARK_W / MARK_H
    start = 60 + mark_width + 50
    lock = mark(60, 40, mark_width) + wordmark(start, 125, 50)
    write(HEADER + svg(round(start + wordmark_width(50) + 60), 200, lock, BG), "assets", "lodestar-lockup.svg")

    save(tile(512, False), "assets", "lodestar-512.png")
    save(tile(1024, False), "assets", "lodestar-1024.png")
    save(tile(512, True), "assets", "icon0.png")
    for size in (16, 32, 48, 128):
        save(small_icon(size), "extension", "icons", f"icon{size}.png")

    # The lockup the native app puts in its header, on a transparent ground.
    brand = Image.new("RGBA", (360, 64), (0, 0, 0, 0))
    paint(brand, mark(4, 10, 50) + wordmark(70, 43, 22))
    path = os.path.join(ROOT, "panel-app", "Brand.png")
    brand.save(path, optimize=True)
    print(f"{os.path.relpath(path, ROOT)}: {brand.width}x{brand.height}")

    # The home screen picture. The console writes its own text on the left and below.
    home = ground(1920, 1080, 1330, 470, 1100)
    paint(home, centred(1330, 420, 420))
    paint(home, wordmark(1330 - wordmark_width(46) / 2, 704, 46))
    save(home, "assets", "pic1.png")


if __name__ == "__main__":
    main()
