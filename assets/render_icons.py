#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Felipe Basurto
"""Renders the whole Lodestar identity from one set of shapes.

    python3 assets/render_icons.py

Only needs Pillow. Writes the mark (a flying wing seen from above carrying an
orange lodestar), the wordmark, the lockup, the PS4 tile (icon0.png) and home
screen art (pic1.png), the browser extension icons and the app's header lockup the
native app draws (panel-app/Backdrop.png and Brand.png).

All art is original. The menu style is inspired by early-2010s military shooter
menus; no game asset, logo or typeface is used. The land mask under the globe is
rasterised from Natural Earth's 110m land (public domain).
"""

import base64
import math
import os
import random
import zlib

from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

BG = "#0a0b0d"
WHITE = "#f3f4f5"
STEEL = "#aab1ba"
ORANGE = "#ff6a13"
SS = 4  # supersampling

LAND = (
    'c-p;H!D<vi5Ut*cnIKChUNnc03Hkx9n8O}|y@HU>Fp#?<UUCTAEjjoZ`U!#`-~=I_M8uOI?1UWRMKh3q6L->WS9NvItmdZJh'
    'e_x4+p1TuYI*?v(fX>iP5(d}>sIDA^8oR5^xaOa&7n@*KV`ybdG+L-BMYQ1Ko#KnqXa`CU<HhVp&<12Nmf{k+rgSkk|wZJBr'
    'yGCvIb?Wz#e9HNwj;aH`8Zs3r!A|S4F4e+M%WW5zzPaJWphi!X$t;JICjpjmo7Sn+RiMo9@6WKy@6zMwC~KRr)qc8#jy|0Q4'
    'O_ubni#f5a$YRZhsn6%89-7)^E@s$H5339uEVC1^-NJ6C={rT56~j@{oVtw%7P_pv@+qzMyJqm2D_il~ZYLO*94M1B&;3VXB'
    'Z5>UA!ka2VG!Bn9icb9wzeXEP}n?zOzo@{)d>TE=Tx2c!d7izz~M#ZRQ;DnNYLo3V-HrPv_S}zILOS$GFDaFRnIukt}y~iCg'
    'tPa>glex!S7`yFdpKoGI$swHX-MvLmzDS$H8*!c`5A_SFQ!@&v&V2QD3PYWT$WRKdMChd6k1-Y=`p73R(Ea5IqjxoxoGxe)L'
    '!;4D#Rx=8uAQajDV4lFgqEDsnNwN36`hw*P)4^MPFi*3481LI^jx%LiGi@ttw(1fl2Kko{>=4vd+qCu@{p`koiSQ;$Z-AD{i'
    'qog7&&`GXlin0H|d{S0j@}$mGk8Ke5;PgPbr=P9b!l_Fs0;KYf^&B2YzCO=$d4*Bv{2{+?8t3j;1dKcP91W;wUvpE@Z2p$(W'
    ')Rzmu~tA^^FGkyY!#polW!Ihi2V*#4|*Sr6EcaThIMz~pWSh_kdh>w7<>m_frx=K+{Gt}eg&7cWH%0snW6E{EBThF>mT>t5r'
    '9J@lUmwl3v6_5<BMzC}%H7;P`5pM2l6UjzQs;HmC#-N>K%Z{T7V&BIUYrh}@^W!+h_vVXsR0jUhduK')


def mirror(half):
    """half: points from the nose (x=0) down the right side to the tail (x=0)."""
    return half + [(-x, y) for x, y in reversed(half[1:-1])]


# The mark, in units of half the wingspan.
RIGHT_HALF = [(0, -0.72), (0.52, -0.04), (1.0, 0.27), (0.985, 0.42), (0.46, 0.24), (0.29, 0.56), (0, 0.38)]
LEFT_HALF = [(-x, y) for x, y in RIGHT_HALF]
STAR = mirror([(0, -0.40), (0.055, -0.13), (0.22, -0.075), (0.055, -0.02), (0, 0.22)])
STAR_SMALL = mirror([(0, -0.46), (0.11, -0.17), (0.34, -0.075), (0.11, 0.02), (0, 0.31)])


def mark(cx, cy, s, flat=False, star=STAR):
    """Shapes of the mark: (colour, polygon). flat drops the two-tone facets."""
    def at(points):
        return [(cx + x * s, cy + (y + 0.08) * s) for x, y in points]
    if flat:
        body = [(WHITE, at(mirror(RIGHT_HALF)))]
    else:
        body = [(WHITE, at(LEFT_HALF)), (STEEL, at(RIGHT_HALF))]
    return body + [(ORANGE, at(star))]


def arc(cx, cy, r, a0, a1, n=8):
    return [(cx + r * math.cos(math.radians(a0 + (a1 - a0) * i / n)),
             cy + r * math.sin(math.radians(a0 + (a1 - a0) * i / n))) for i in range(n + 1)]


# The wordmark: wide squared capitals, 100 units high, strokes 19 units.
def _letters():
    o_out = (arc(24, 24, 24, 180, 270) + arc(72, 24, 24, 270, 360)
             + arc(72, 76, 24, 0, 90) + arc(24, 76, 24, 90, 180))
    o_in = (arc(27, 27, 8, 180, 270) + arc(69, 27, 8, 270, 360)
            + arc(69, 73, 8, 0, 90) + arc(27, 73, 8, 90, 180))
    d_out = [(0, 0)] + arc(62, 30, 30, 270, 360) + arc(62, 70, 30, 0, 90) + [(0, 100)]
    d_in = [(19, 19)] + arc(62, 30, 11, 270, 360) + arc(62, 70, 11, 0, 90) + [(19, 81)]
    s = ([(86, 0)] + arc(22, 22, 22, 270, 180) + arc(22, 37, 22, 180, 90) + [(67, 59), (67, 81), (0, 81), (0, 100)]
         + arc(64, 78, 22, 90, 0) + arc(64, 63, 22, 0, -90) + [(19, 41), (19, 19), (86, 19)])
    r_out = [(0, 0)] + arc(66, 24, 24, 270, 360) + arc(66, 40, 24, 0, 90) + [(66, 64), (91, 100), (68, 100), (43, 64),
                                                                             (19, 64), (19, 100), (0, 100)]
    r_in = [(19, 19)] + arc(66, 25, 6, 270, 360) + arc(66, 39, 6, 0, 90) + [(19, 45)]
    return {
        "L": (76, [[(0, 0), (19, 0), (19, 81), (76, 81), (76, 100), (0, 100)]]),
        "O": (96, [o_out, o_in]),
        "D": (92, [d_out, d_in]),
        "E": (78, [[(0, 0), (78, 0), (78, 19), (19, 19), (19, 41), (70, 41), (70, 59), (19, 59), (19, 81),
                    (78, 81), (78, 100), (0, 100)]]),
        "S": (86, [s]),
        "T": (82, [[(0, 0), (82, 0), (82, 19), (50.5, 19), (50.5, 100), (31.5, 100), (31.5, 19), (0, 19)]]),
        "A": (100, [[(0, 100), (39, 0), (61, 0), (100, 100), (79, 100), (50, 25), (21, 100)]]),
        "R": (91, [r_out, r_in]),
    }


LETTERS = _letters()
GAP = 17
WORD = "LODESTAR"
WORD_WIDTH = sum(LETTERS[ch][0] for ch in WORD) + GAP * (len(WORD) - 1)


def wordmark(x, y, height):
    """(colour, [outline, counter...]) per letter. The crossbarless A is orange."""
    k = height / 100
    out = []
    for ch in WORD:
        width, rings = LETTERS[ch]
        out.append((ORANGE if ch == "A" else WHITE, [[(x + px * k, y + py * k) for px, py in ring] for ring in rings]))
        x += (width + GAP) * k
    return out


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
    """Antialiased fill of the shapes (even-odd rings) on an RGBA image."""
    big = (image.width * SS, image.height * SS)
    by_colour = {}
    for colour, shape in shapes:
        rings = shape if isinstance(shape[0], list) else [shape]
        mask = by_colour.setdefault(colour, Image.new("L", big, 0))
        d = ImageDraw.Draw(mask)
        for i, ring in enumerate(rings):
            d.polygon([(x * SS, y * SS) for x, y in ring], fill=0 if i else 255)
    # The two facets of the wing meet on a shared edge: join their masks first
    # so that no background shows through the seam.
    if WHITE in by_colour and STEEL in by_colour:
        both = Image.new("L", big, 0)
        both.paste(255, mask=by_colour[WHITE])
        both.paste(255, mask=by_colour[STEEL])
        image.paste(rgb(WHITE) + (255,), mask=both.resize(image.size, Image.LANCZOS))
        del by_colour[WHITE]
    for colour, mask in by_colour.items():
        image.paste(rgb(colour) + (255,), mask=mask.resize(image.size, Image.LANCZOS))
    return image


def honeycomb(image, cell, alpha, fade=lambda x, y: 1.0):
    """A faint hexagon mesh, the surface the menu sits on."""
    layer = Image.new("L", (image.width * 2, image.height * 2), 0)
    d = ImageDraw.Draw(layer)
    r = cell * 2
    step_x, step_y = math.sqrt(3) * r, 1.5 * r
    for row in range(int(layer.height / step_y) + 2):
        for col in range(int(layer.width / step_x) + 2):
            cx = col * step_x + (row % 2) * step_x / 2
            cy = row * step_y
            strength = fade(cx / 2, cy / 2)
            if strength <= 0:
                continue
            pts = [(cx + r * math.cos(math.radians(a)), cy + r * math.sin(math.radians(a)))
                   for a in range(-90, 270, 60)]
            d.polygon(pts, outline=int(alpha * min(strength, 1.0)), width=3)
    image.paste((255, 255, 255, 255), mask=layer.resize(image.size, Image.LANCZOS))


def land(lon, lat):
    bits = land.bits
    x = int((lon + 180) / 2) % 180
    y = min(89, max(0, int((90 - lat) / 2)))
    n = y * 180 + x
    return bits[n >> 3] >> (n & 7) & 1


land.bits = zlib.decompress(base64.b85decode("".join(LAND)))


def globe(image, cx, cy, radius, lon0=-48.0, lat0=24.0):
    """A dot-matrix night globe with orange city lights."""
    k = 2
    layer = Image.new("RGBA", (image.width * k, image.height * k), (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    d.ellipse([(cx - radius) * k, (cy - radius) * k, (cx + radius) * k, (cy + radius) * k],
              fill=(8, 9, 11, 235), outline=(110, 116, 126, 70), width=2 * k)
    rand = random.Random(1500)
    sin0, cos0 = math.sin(math.radians(lat0)), math.cos(math.radians(lat0))
    step = 1.15
    lat = -88.0
    while lat < 88:
        lat += step
        lon = -180.0
        lon_step = step / max(0.12, math.cos(math.radians(lat)))
        while lon < 180:
            lon += lon_step
            if not land(lon, lat):
                continue
            phi, lam = math.radians(lat), math.radians(lon - lon0)
            z = sin0 * math.sin(phi) + cos0 * math.cos(phi) * math.cos(lam)
            if z <= 0.03:
                continue
            px = cx + radius * math.cos(phi) * math.sin(lam)
            py = cy - radius * (cos0 * math.sin(phi) - sin0 * math.cos(phi) * math.cos(lam))
            coast = not (land(lon + 3, lat) and land(lon - 3, lat) and land(lon, lat + 3) and land(lon, lat - 3))
            roll = rand.random()
            size = radius * 0.0042
            if roll < (0.20 if coast else 0.035):
                colour, size = (255, 150 + rand.randrange(60), 40, int(130 + 125 * z)), size * (1.1 + rand.random())
                d.ellipse([(px - size * 3) * k, (py - size * 3) * k, (px + size * 3) * k, (py + size * 3) * k],
                          fill=(255, 106, 19, int(26 * z)))
            else:
                colour = (160, 96, 62, int(40 + 90 * z))
            d.ellipse([(px - size) * k, (py - size) * k, (px + size) * k, (py + size) * k], fill=colour)
    image.alpha_composite(layer.resize(image.size, Image.LANCZOS))


def floor(image, horizon, vanish_x):
    """Perspective field of small crosses, a few of them lit orange."""
    k = 2
    layer = Image.new("RGBA", (image.width * k, image.height * k), (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    rand = random.Random(47)
    for row in range(1, 18):
        near = row / 16.0
        y = horizon + (image.height - horizon) * near ** 1.8
        if y > image.height:
            break
        spread = 34 + (y - horizon) * 0.66
        arm = 4 + (y - horizon) * 0.032
        for col in range(-40, 41):
            x = vanish_x + col * spread
            if not -20 < x < image.width + 20:
                continue
            lit = rand.random() < 0.045
            colour = (255, 106, 19, 240) if lit else (205, 210, 218, int(36 + 90 * min(1.0, near)))
            width = max(2, int(arm * (0.36 if lit else 0.22) * k))
            d.line([(x - arm) * k, y * k, (x + arm) * k, y * k], fill=colour, width=width)
            d.line([x * k, (y - arm * 0.6) * k, x * k, (y + arm * 0.6) * k], fill=colour, width=width)
    image.alpha_composite(layer.resize(image.size, Image.LANCZOS))


def vertical_light(image, colour, x, y, rx, ry, alpha):
    """A soft elliptical light."""
    small = Image.new("L", (192, 108), 0)
    px = small.load()
    for j in range(108):
        for i in range(192):
            dx = (i / 192 * image.width - x) / rx
            dy = (j / 108 * image.height - y) / ry
            px[i, j] = int(alpha * max(0.0, 1 - math.sqrt(dx * dx + dy * dy)) ** 1.6)
    image.paste(rgb(colour) + (255,), mask=small.resize(image.size, Image.BICUBIC))


def backdrop(width=1920, height=1080, globe_at=(150, 600, 520)):
    image = Image.new("RGBA", (width, height), rgb(BG) + (255,))
    vertical_light(image, "#2a2f37", width * 0.72, height * 0.18, width * 0.55, height * 0.8, 255)
    honeycomb(image, 13, 15, lambda x, y: (x / width) * 1.2 - 0.1)
    floor(image, height * 0.70, width * 0.56)
    globe(image, *globe_at)
    return image


def tile(size, wordmarked):
    """The square app art."""
    image = Image.new("RGBA", (size, size), rgb(BG) + (255,))
    vertical_light(image, "#2b3038", size * 0.5, size * 0.34, size * 0.72, size * 0.72, 255)
    honeycomb(image, size / 38, 16)
    k = size / 512
    if wordmarked:
        paint(image, mark(256 * k, 214 * k, 212 * k))
        height = 452 * k / WORD_WIDTH * 100
        paint(image, wordmark(30 * k, 398 * k, height))
        paint(image, [(ORANGE, [(0, 484 * k), (512 * k, 484 * k), (512 * k, 512 * k), (0, 512 * k)])])
    else:
        paint(image, mark(256 * k, 268 * k, 222 * k))
    return image


def small_icon(size):
    """Extension and favicon sizes: one flat wing and a fatter star on a dark square."""
    image = Image.new("RGBA", (size, size), rgb(BG) + (255,))
    paint(image, mark(size / 2, size * 0.52, size * 0.47, flat=size < 48, star=STAR_SMALL))
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
    write(HEADER + svg(512, 512, mark(256, 268, 222), BG), "assets", "lodestar.svg")
    write(HEADER + svg(WORD_WIDTH, 100, wordmark(0, 0, 100)), "assets", "lodestar-wordmark.svg")
    lock = mark(150, 100, 112) + wordmark(300, 66, 68)
    write(HEADER + svg(300 + WORD_WIDTH * 0.68 + 44, 200, lock, BG), "assets", "lodestar-lockup.svg")

    save(tile(512, False), "assets", "lodestar-512.png")
    save(tile(1024, False), "assets", "lodestar-1024.png")
    save(tile(512, True), "assets", "icon0.png")
    for size in (16, 32, 48, 128):
        save(small_icon(size), "extension", "icons", f"icon{size}.png")

    # The lockup the native app puts in its header, on a transparent ground.
    brand = Image.new("RGBA", (360, 64), (0, 0, 0, 0))
    paint(brand, mark(44, 31, 43) + wordmark(100, 19, 26))
    path = os.path.join(ROOT, "panel-app", "Brand.png")
    brand.save(path, optimize=True)
    print(f"{os.path.relpath(path, ROOT)}: {brand.width}x{brand.height}")
    home = backdrop(globe_at=(340, 640, 600))
    paint(home, mark(1330, 430, 270))
    paint(home, wordmark(1330 - WORD_WIDTH * 0.72 / 2, 640, 72))
    save(home, "assets", "pic1.png")


if __name__ == "__main__":
    main()
