#!/usr/bin/python3 -I
# build_renaissance.py OUTDIR SRCDIR: the "renaissance-cyber" parallax set for Neo ("Cyberpunk meets Italian
# Renaissance"), v3: a granite statue owl of Minerva, cut open to platinum, gold wire and gears, with Kiroshi camera
# eyes (the only warm light), in front of a gear ring and a glitch-sliced Vitruvian Man (Leonardo, public domain,
# Wikimedia Commons "Da_Vinci_Vitruve_Luc_Viatour.jpg", lifted to silver line-work), granite-and-platinum corner
# brackets in front. All on OLED black; nothing here animates.
#
# SRCDIR holds the sources (not in the repository; the owl art is AI generated for the owner, see docs/theme.md):
#   p1_owl.png      the statue owl on flat black (9:16)
#   p3_ring.png     the gear ring, empty centre (1:1)
#   p4_corner.png   a corner bracket in the top-left corner of an otherwise black square (1:1)
#   vitruvian_ink.png   the Vitruvian Man as an 8-bit ink mask (white = ink), from tools/parallax-renaissance/vitruvian_ink.py
# Colours: tools/theme/palette.json (Minerva). Needs python3-pil and python3-numpy. Layer conventions as in
# tools/parallax-solar-system (square layers, depth = parallax factor).
import json
import math
import os
import sys

import numpy as np
from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont

OUT, SRC = sys.argv[1], sys.argv[2]
N = 2340
SW, SH, SCALE = 1080, 2400, 3
RANGE, TILT = 60, 0.2
DEPTHS = [-0.6, -0.25, 0.1, 0.55, 1.0]
rng = np.random.default_rng(20261114)

BG = (7, 6, 11)
SILVER = (176, 181, 194)
SILVER_DIM = (110, 114, 128)
GOLD = (217, 178, 106)
AMBER = (255, 159, 46)
MONO = "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf"   # JetBrains Mono is on the phone, not on the build host


def zoom(depth):
    return (SH + 2 * RANGE * abs(depth) * SCALE) / N


def blank():
    return Image.new("RGBA", (N, N), (0, 0, 0, 0))


def key(im, seeds, thr=10):
    """flat-black background -> alpha: the black connected to the seeds goes, dark stone inside the figure stays"""
    rgb = im.convert("RGB")
    dark = np.asarray(rgb).max(axis=2) < thr
    bg = np.zeros_like(dark)
    for x, y in seeds:
        if dark[y, x]:
            bg[y, x] = True
    while True:                      # flood fill: grow the seeds through the dark pixels, 4-connected
        grown = bg.copy()
        grown[1:] |= bg[:-1]
        grown[:-1] |= bg[1:]
        grown[:, 1:] |= bg[:, :-1]
        grown[:, :-1] |= bg[:, 1:]
        grown &= dark
        if (grown == bg).all():
            break
        bg = grown
    alpha = Image.fromarray(np.where(bg, 0, 255).astype(np.uint8))
    alpha = alpha.filter(ImageFilter.MinFilter(3)).filter(ImageFilter.GaussianBlur(1.1))
    out = rgb.convert("RGBA")
    out.putalpha(alpha)
    return out


def place(layer, src, depth, cx, cy, w=None, h=None, flip_x=False, flip_y=False):
    """paste src into the layer so that it shows w (or h) screen pixels wide (tall), centred on screen point cx, cy"""
    s = zoom(depth)
    if w is not None:
        tw = w / s
        th = tw * src.height / src.width
    else:
        th = h / s
        tw = th * src.width / src.height
    im = src.resize((max(1, round(tw)), max(1, round(th))), Image.LANCZOS)
    if flip_x:
        im = im.transpose(Image.FLIP_LEFT_RIGHT)
    if flip_y:
        im = im.transpose(Image.FLIP_TOP_BOTTOM)
    x = N / 2 + (cx - SW / 2) / s - im.width / 2
    y = N / 2 + (cy - SH / 2) / s - im.height / 2
    layer.alpha_composite(im, (round(x), round(y))) if x >= 0 and y >= 0 else layer.paste(im, (round(x), round(y)), im)


def scaled(im, k):
    a = im.getchannel("A").point(lambda v: int(v * k))
    out = im.copy()
    out.putalpha(a)
    return out


def layer0():
    yy, xx = np.mgrid[0:N, 0:N].astype(np.float32)
    t = (yy / N)[..., None]
    img = np.array([9, 8, 14], np.float32) * (1 - t) + np.array([5, 4, 8], np.float32) * t
    # a cool glow behind the statue, a dim stone floor line low on the screen
    g = np.exp(-(((xx - N / 2) / (N * 0.30)) ** 2 + ((yy - N * 0.47) / (N * 0.34)) ** 2))[..., None]
    img += g * np.array([14, 15, 24], np.float32)
    im = Image.fromarray(np.clip(img, 0, 255).astype(np.uint8)).convert("RGBA")
    d = ImageDraw.Draw(im, "RGBA")
    # dotwork: fine stipple falling off from the centre
    for _ in range(5200):
        x, y = rng.normal(N / 2, N * 0.30), rng.normal(N * 0.47, N * 0.34)
        if 0 <= x < N and 0 <= y < N:
            r = rng.choice([1, 1, 1, 2])
            d.ellipse([x - r, y - r, x + r, y + r], fill=SILVER + (int(rng.integers(30, 120)),))
    return im.convert("RGB")


def glitch_vitruvian():
    ink = Image.open(os.path.join(SRC, "vitruvian_ink.png")).convert("L")
    w = 1180
    h = round(w * ink.height / ink.width)
    ink = ink.resize((w, h), Image.LANCZOS)
    col = Image.new("RGBA", (w, h), SILVER + (0,))
    col.putalpha(ink.point(lambda v: int(v * 1.0)))
    out = Image.new("RGBA", (w + 260, h), (0, 0, 0, 0))
    y = 0
    while y < h:
        sh = int(rng.integers(14, 70))
        strip = col.crop((0, y, w, min(h, y + sh)))
        dx = 0
        if rng.random() < 0.28:
            dx = int(rng.choice([-1, 1]) * rng.integers(8, 50))
        out.alpha_composite(strip, (130 + dx, y))
        if dx and rng.random() < 0.35:     # the one warm glitch edge
            edge = Image.new("RGBA", strip.size, AMBER + (0,))
            edge.putalpha(strip.getchannel("A").point(lambda v: int(v * 0.5)))
            out.alpha_composite(edge, (130 + dx + 3, y))
        y += sh
    return out


def corner_bracket():
    im = Image.open(os.path.join(SRC, "p4_corner.png")).convert("RGB")
    big = key(im, [(im.width - 3, im.height - 3), (im.width - 3, 3), (3, im.height - 3)])
    box = big.getchannel("A").point(lambda v: 255 if v > 24 else 0).getbbox()
    return big.crop(box)


def hud(layer):
    """tiny HUD microtext: a row of squares and a tick bar, JetBrains Mono's look in DejaVu Mono on the build host"""
    d = ImageDraw.Draw(layer, "RGBA")
    s = zoom(DEPTHS[4])

    def pt(x, y):
        return N / 2 + (x - SW / 2) / s, N / 2 + (y - SH / 2) / s

    f = ImageFont.truetype(MONO, round(15 / s * 1.0))
    x0, y0 = pt(390, 2330)
    d.text((x0, y0), "MINERVA  //  OCULUS ACTIVE  //  MMXXVI", font=f, fill=SILVER_DIM + (180,))
    for i in range(24):                           # squares
        x, y = pt(390 + i * 13, 2356)
        on = rng.random() < 0.55
        d.rectangle([x, y, x + 6 / s, y + 6 / s], fill=(AMBER if i == 3 else SILVER_DIM) + (200 if on else 70,))
    for i in range(61):                           # tick bar
        x, y = pt(700 + i * 5, 2362)
        d.line([x, y, x, y - (14 if i % 10 == 0 else 7 if i % 5 == 0 else 4) / s], fill=SILVER_DIM + (170,), width=1)


def main():
    os.makedirs(OUT, exist_ok=True)
    l0 = layer0()

    l2 = blank()
    place(l2, glitch_vitruvian(), DEPTHS[2], 540, 1170, w=1620)

    l1 = blank()
    ring = Image.open(os.path.join(SRC, "p3_ring.png")).convert("RGB").resize((1400, 1400), Image.LANCZOS)
    ring = key(ring, [(3, 3), (700, 700)], thr=7)
    place(l1, scaled(ring, 0.34), DEPTHS[1], 540, 1160, w=1420)

    l3 = blank()
    owl = Image.open(os.path.join(SRC, "p1_owl.png")).convert("RGB")
    owl = owl.resize((round(owl.width * 1500 / owl.height), 1500), Image.LANCZOS)
    owl = key(owl, [(3, 3), (owl.width - 4, 3), (3, owl.height - 4), (owl.width - 4, owl.height - 4)])
    place(l3, scaled(owl, 0.92), DEPTHS[3], 540, 1330, h=1500)

    l4 = blank()
    cb = corner_bracket()
    for fx, fy, x, y in ((False, False, 190, 300), (True, False, SW - 190, 300), (False, True, 190, SH - 190),
                         (True, True, SW - 190, SH - 190)):
        place(l4, scaled(cb, 0.85), DEPTHS[4], x, y, w=330, flip_x=fx, flip_y=fy)
    hud(l4)

    layers = [l0.convert("RGBA"), l1, l2, l3, l4]
    l0.save(os.path.join(OUT, "layer0.jpg"), quality=90)
    for i in range(1, 5):
        layers[i].save(os.path.join(OUT, f"layer{i}.png"), optimize=True)
    json.dump({"range": RANGE, "tilt": TILT, "layers": [
        {"file": "layer0.jpg", "depth": DEPTHS[0]},
        *[{"file": f"layer{i}.png", "depth": DEPTHS[i]} for i in range(1, 5)]]},
        open(os.path.join(OUT, "parallax.json"), "w"), indent=2)

    wall = Image.new("RGBA", (SW, SH), BG + (255,))
    for im, depth in zip(layers, DEPTHS):
        m = math.ceil(RANGE * abs(depth) * SCALE)
        bw, bh = SW + 2 * m, SH + 2 * m
        side = max(bw, bh)
        big = im.resize((side, side), Image.LANCZOS)
        big = big.crop(((side - bw) // 2, (side - bh) // 2, (side - bw) // 2 + bw, (side - bh) // 2 + bh))
        wall.alpha_composite(big, (-m, -m))
    wall = wall.convert("RGB")
    wall.save(os.path.join(OUT, "wallpaper.jpg"), quality=92)
    wall.resize((540, 1200), Image.LANCZOS).save(os.path.join(OUT, "preview.jpg"), quality=88)
    print("wrote", OUT)


main()
