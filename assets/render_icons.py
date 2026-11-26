# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Felipe Basurto
#!/usr/bin/env python3
"""Renders the Lodestar icon PNGs from the same geometry as lodestar.svg.

Only uses PIL + numpy (both already installed). Run from anywhere:

    python3 assets/render_icons.py

Writes next to this script:
    lodestar-512.png, lodestar-1024.png, icon0.png (512x512, packaging artifact)

The drawing is done at 2048x2048 (4x the 512 design grid) and downscaled
with LANCZOS, which keeps edges clean at small sizes.
"""

import math
import os

import numpy as np
from PIL import Image, ImageDraw

GRID = 512          # design grid, matches the SVG viewBox
RENDER = 2048       # supersampled working size
S = RENDER / GRID   # scale factor for coordinates

HERE = os.path.dirname(os.path.abspath(__file__))


def hexrgb(h):
    h = h.lstrip("#")
    return tuple(int(h[i:i + 2], 16) for i in (0, 2, 4))


def vertical_gradient(size, stops):
    """stops: list of (offset 0..1, hex color). Returns HxWx3 float array."""
    arr = np.zeros((size, size, 3), dtype=np.float64)
    ys = np.linspace(0.0, 1.0, size)
    cols = np.zeros((size, 3))
    for i, y in enumerate(ys):
        # find the two stops around y
        for j in range(len(stops) - 1):
            o0, c0 = stops[j]
            o1, c1 = stops[j + 1]
            if o0 <= y <= o1:
                t = 0.0 if o1 == o0 else (y - o0) / (o1 - o0)
                a, b = np.array(hexrgb(c0)), np.array(hexrgb(c1))
                cols[i] = a + (b - a) * t
                break
        else:
            cols[i] = hexrgb(stops[-1][1])
    arr[:, :, :] = cols[:, None, :]
    return arr


def rounded_rect_mask(size, radius):
    m = Image.new("L", (size, size), 0)
    d = ImageDraw.Draw(m)
    d.rounded_rectangle([0, 0, size - 1, size - 1], radius=radius, fill=255)
    return m


def radial_glow(size, cx, cy, radius, color, peak_alpha):
    """Radial falloff layer as HxWx4 array (alpha 0 at radius)."""
    yy, xx = np.mgrid[0:size, 0:size].astype(np.float64)
    dist = np.sqrt((xx - cx) ** 2 + (yy - cy) ** 2) / radius
    falloff = np.clip(1.0 - dist, 0.0, 1.0) ** 2
    alpha = falloff * peak_alpha * 255.0
    layer = np.zeros((size, size, 4), dtype=np.float64)
    layer[:, :, 0], layer[:, :, 1], layer[:, :, 2] = hexrgb(color)
    layer[:, :, 3] = alpha
    return layer


def alpha_over(base_arr, layer_arr):
    """Composite RGBA float layer over RGB float base."""
    a = layer_arr[:, :, 3:4] / 255.0
    base_arr[:, :, :] = base_arr * (1 - a) + layer_arr[:, :, :3] * a
    return base_arr


def qbez(p0, c, p1, n=64):
    """Sample a quadratic bezier."""
    pts = []
    for i in range(n + 1):
        t = i / n
        x = (1 - t) ** 2 * p0[0] + 2 * (1 - t) * t * c[0] + t ** 2 * p1[0]
        y = (1 - t) ** 2 * p0[1] + 2 * (1 - t) * t * c[1] + t ** 2 * p1[1]
        pts.append((x, y))
    return pts


def sc(points):
    """Scale design-grid coordinates to the render surface."""
    return [(x * S, y * S) for x, y in points]


def star_polygon(cx=256, cy=256, tip=160, waist=32):
    """Four-point sparkle: tips on the axes, concave quadratic edges.

    waist is the control-point offset from the center along each diagonal.
    """
    n = (cx, cy - tip)
    e = (cx + tip, cy)
    s = (cx, cy + tip)
    w = (cx - tip, cy)
    cne = (cx + waist, cy - waist)
    cse = (cx + waist, cy + waist)
    csw = (cx - waist, cy + waist)
    cnw = (cx - waist, cy - waist)
    poly = []
    for a, c, b in ((n, cne, e), (e, cse, s), (s, csw, w), (w, cnw, n)):
        poly.extend(qbez(a, c, b, n=48)[:-1])
    return poly


def main():
    # ---- base tile: vertical navy gradient inside a rounded rect ----
    bg = vertical_gradient(RENDER, [
        (0.0, "#16223a"),
        (0.55, "#0d1526"),
        (1.0, "#060b15"),
    ])
    mask = rounded_rect_mask(RENDER, 112 * S)

    # amber glow behind the star
    glow = radial_glow(RENDER, RENDER * 0.5, RENDER * 0.46,
                       RENDER * 0.52, "#f5a524", 0.20)
    alpha_over(bg, glow)

    img = Image.fromarray(bg.astype(np.uint8), "RGB").convert("RGBA")
    img.putalpha(mask)

    def overlay():
        layer = Image.new("RGBA", (RENDER, RENDER), (0, 0, 0, 0))
        return layer, ImageDraw.Draw(layer)

    def paste(layer):
        img.alpha_composite(layer)

    # ---- subtle inner border ----
    layer, d = overlay()
    d.rounded_rectangle([5 * S, 5 * S, 507 * S, 507 * S], radius=107 * S,
                        outline=(255, 255, 255, 15), width=max(1, round(3 * S)))
    paste(layer)

    # ---- compass ring ----
    layer, d = overlay()
    teal = hexrgb("#5fd3c7")
    amber = hexrgb("#f5a524")
    r = 172 * S
    c = RENDER / 2
    d.ellipse([c - r, c - r, c + r, c + r],
              outline=teal + (97,), width=round(7 * S))  # ~38% alpha
    paste(layer)

    # ---- ticks ----
    # cardinal ticks cross the ring: in the SVG they run from radius 192
    # down to 166; angles measured from the +x axis, north is -90
    layer, d = overlay()
    for ang, width, col, al in (
        (-90, 10, amber, 255),
        (90, 8, teal, 178),
        (180, 8, teal, 178),
        (0, 8, teal, 178),
    ):
        a = math.radians(ang)
        rr0 = 192 * S
        rr1 = 166 * S
        x0 = c + rr0 * math.cos(a)
        y0 = c + rr0 * math.sin(a)
        x1 = c + rr1 * math.cos(a)
        y1 = c + rr1 * math.sin(a)
        d.line([x0, y0, x1, y1], fill=col + (al,), width=round(width * S))
    # intercardinal small ticks at radius ~192..169 rotated 45 deg
    for ang in (-45, 45, 135, -135):
        a = math.radians(ang)
        rr0, rr1 = 190 * S, 167 * S
        x0 = c + rr0 * math.cos(a)
        y0 = c + rr0 * math.sin(a)
        x1 = c + rr1 * math.cos(a)
        y1 = c + rr1 * math.sin(a)
        d.line([x0, y0, x1, y1], fill=teal + (89,), width=round(6 * S))
    paste(layer)

    # ---- the lodestar sparkle, filled with the amber vertical gradient ----
    poly = sc(star_polygon())
    star_mask = Image.new("L", (RENDER, RENDER), 0)
    ImageDraw.Draw(star_mask).polygon(poly, fill=255)

    grad = vertical_gradient(RENDER, [
        (0.0, "#ffd88a"),
        (0.45, "#f5a524"),
        (1.0, "#d97f06"),
    ])
    star_img = Image.fromarray(grad.astype(np.uint8), "RGB").convert("RGBA")
    star_img.putalpha(star_mask)
    img.alpha_composite(star_img)

    # slim highlight on the north point (same shape as the SVG path)
    layer, d = overlay()
    hl = qbez((256, 96), (272, 200), (280, 228), n=32) + \
         qbez((280, 228), (264, 204), (256, 120), n=32) + \
         qbez((256, 120), (250, 106), (256, 96), n=8)
    d.polygon(sc(hl), fill=(255, 255, 255, 56))
    paste(layer)

    # ---- the core: dark cutout + teal center ----
    layer, d = overlay()
    rr = 34 * S
    d.ellipse([c - rr, c - rr, c + rr, c + rr], fill=hexrgb("#0a1120") + (255,))
    paste(layer)
    core_grad = vertical_gradient(RENDER, [
        (0.0, "#8ff0e4"),
        (1.0, "#3ab3a6"),
    ])
    core_mask = Image.new("L", (RENDER, RENDER), 0)
    rr = 22 * S
    ImageDraw.Draw(core_mask).ellipse([c - rr, c - rr, c + rr, c + rr], fill=255)
    core_img = Image.fromarray(core_grad.astype(np.uint8), "RGB").convert("RGBA")
    core_img.putalpha(core_mask)
    img.alpha_composite(core_img)

    # ---- flatten onto transparency-free RGB for the packaging artifact ----
    out512 = img.resize((512, 512), Image.LANCZOS)
    out1024 = img.resize((1024, 1024), Image.LANCZOS)

    out512.save(os.path.join(HERE, "lodestar-512.png"))
    out1024.save(os.path.join(HERE, "lodestar-1024.png"))
    # icon0.png is the artifact build.sh embeds and panel-app copies into sce_sys;
    # keep it RGB (no alpha) as the packaging tools expect.
    out512.convert("RGB").save(os.path.join(HERE, "icon0.png"))

    for name in ("lodestar-512.png", "lodestar-1024.png", "icon0.png"):
        im = Image.open(os.path.join(HERE, name))
        print(f"{name}: {im.size} {im.mode}")


if __name__ == "__main__":
    main()
