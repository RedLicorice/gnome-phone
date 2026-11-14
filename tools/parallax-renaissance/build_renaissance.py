#!/usr/bin/python3 -I
# build_renaissance.py OUTDIR: the "renaissance-cyber" parallax set for Neo ("Cyberpunk meets Italian Renaissance"),
# all procedural (no third-party material): a night of lapis and gold dust, an armillary sphere, Vitruvian circle and
# square, an owl of Minerva framed by laurel, a Brunelleschi arcade, neon cyan and magenta glitch bars in front.
# Needs python3-pil and python3-numpy. Same layer conventions as tools/parallax-solar-system.
import json
import math
import os
import sys

import numpy as np
from PIL import Image, ImageChops, ImageDraw, ImageFilter

OUT = sys.argv[1]
N = 2340
SW, SH, SCALE = 1080, 2400, 3
RANGE, TILT = 60, 0.2
DEPTHS = [-0.6, -0.25, 0.1, 0.55, 1.0]
SS = 2
rng = np.random.default_rng(20261114)

GOLD = (217, 178, 106)
GOLD_HI = (243, 220, 159)
CYAN = (56, 232, 255)
MAGENTA = (255, 59, 212)
LAPIS = (35, 65, 143)


def zoom(depth):
    return (SH + 2 * RANGE * abs(depth) * SCALE) / N


def to_layer(depth, x, y, d=0.0):
    """a point (and a length d) in screen pixels -> in the layer square, times SS for the drawing canvas"""
    s = zoom(depth)
    return (N / 2 + (x - SW / 2) / s) * SS, (N / 2 + (y - SH / 2) / s) * SS, d / s * SS


class Canvas:
    def __init__(self, depth, bg=(0, 0, 0, 0)):
        self.depth = depth
        self.im = Image.new("RGBA", (N * SS, N * SS), bg)
        self.d = ImageDraw.Draw(self.im, "RGBA")

    def p(self, x, y):
        return to_layer(self.depth, x, y)[:2]

    def w(self, px):
        return max(1, round(to_layer(self.depth, 0, 0, px)[2]))

    def line(self, pts, col, a, width):
        self.d.line([self.p(*q) for q in pts], fill=col + (round(255 * a),), width=self.w(width), joint="curve")

    def circle(self, cx, cy, r, col, a, width, fill=None):
        x, y = self.p(cx, cy)
        rr = to_layer(self.depth, 0, 0, r)[2]
        self.d.ellipse((x - rr, y - rr, x + rr, y + rr), outline=col + (round(255 * a),) if width else None,
                       width=self.w(width) if width else 0, fill=fill)

    def arc(self, cx, cy, r, a0, a1, col, a, width):
        x, y = self.p(cx, cy)
        rr = to_layer(self.depth, 0, 0, r)[2]
        self.d.arc((x - rr, y - rr, x + rr, y + rr), a0, a1, fill=col + (round(255 * a),), width=self.w(width))

    def glow(self, radius, strength=1.0):
        """add a blurred copy under the sharp lines: the neon"""
        blur = self.im.filter(ImageFilter.GaussianBlur(to_layer(self.depth, 0, 0, radius)[2]))
        blur = Image.eval(blur.split()[3], lambda v: min(255, int(v * strength)))
        glow = Image.merge("RGBA", (*self.im.split()[:3], blur))
        self.im = Image.alpha_composite(glow, self.im)
        self.d = ImageDraw.Draw(self.im, "RGBA")

    def done(self):
        return self.im.resize((N, N), Image.LANCZOS)


# ---- layer 0: the night: gradient, dawn glow, gold dust, armillary rings, a thin cyan perspective floor -------
def layer0():
    depth = DEPTHS[0]
    h = N
    yy, xx = np.mgrid[0:h, 0:N].astype(np.float32)
    top = np.array([10, 9, 30], np.float32)
    bot = np.array([4, 3, 9], np.float32)
    t = (yy / h)[..., None]
    img = top * (1 - t) + bot * t
    # a warm glow behind the emblem, a cool one low in the corner
    cx, cy = N / 2, N * 0.42
    g = np.exp(-(((xx - cx) / (N * 0.38)) ** 2 + ((yy - cy) / (N * 0.30)) ** 2))[..., None]
    img += g * np.array([34, 22, 40], np.float32)
    g2 = np.exp(-(((xx - N * 0.1) / (N * 0.3)) ** 2 + ((yy - N * 0.92) / (N * 0.2)) ** 2))[..., None]
    img += g2 * np.array([8, 40, 60], np.float32)
    im = Image.fromarray(np.clip(img, 0, 255).astype(np.uint8)).convert("RGBA")
    # gold dust
    d = ImageDraw.Draw(im, "RGBA")
    for _ in range(1100):
        x, y = rng.uniform(0, N), rng.uniform(0, N)
        r = rng.choice([1, 1, 1, 1.5, 2, 3])
        a = int(rng.uniform(60, 220))
        col = GOLD_HI if rng.random() < 0.6 else (190, 210, 255)
        d.ellipse((x - r, y - r, x + r, y + r), fill=col + (a,))
    # armillary rings: thin gold ellipses, tilted by drawing them on a scratch canvas and rotating
    ring = Image.new("RGBA", (N * 2, N * 2), (0, 0, 0, 0))
    rd = ImageDraw.Draw(ring, "RGBA")
    for rx, ry, a in [(980, 980, 70), (980, 360, 80), (760, 760, 55), (980, 640, 60)]:
        rd.ellipse((N - rx, N - ry, N + rx, N + ry), outline=GOLD + (a,), width=3)
    ring = ring.rotate(-22, resample=Image.BICUBIC).crop((N // 2, N // 2, N // 2 + N, N // 2 + N))
    im.alpha_composite(ring)
    # a cyan perspective floor, very faint, under the arcade
    fl = Canvas(depth)
    horizon = 1750
    for i in range(-14, 15):
        fl.line([(540 + i * 40, horizon), (540 + i * 330, 2600)], CYAN, 0.16, 1.5)
    for k in range(1, 9):
        y = horizon + (k / 8) ** 2.2 * 650
        fl.line([(-200, y), (1280, y)], CYAN, 0.16, 1.5)
    fl.glow(6, 0.8)
    floor = fl.done()
    im.alpha_composite(floor)
    return im.convert("RGB")


# ---- layer 1: Vitruvian circle and square, golden pentagon, graduated ticks, cyan nodes ----------------------
def layer1():
    c = Canvas(DEPTHS[1])
    cx, cy, R = 540, 1010, 400
    c.circle(cx, cy, R, GOLD, 0.62, 3)
    c.circle(cx, cy, R * 1.06, GOLD, 0.25, 1.5)
    s = R * math.sqrt(2) / 2 * 1.0
    # the square, its base tangent to the circle's lower part (Vitruvian: square below the circle's centre)
    sq = R * 0.92
    top = cy - sq * 0.55
    c.line([(cx - sq, top), (cx + sq, top), (cx + sq, top + 2 * sq), (cx - sq, top + 2 * sq), (cx - sq, top)], GOLD, 0.5, 2.5)
    # ticks around the circle
    for deg in range(0, 360, 5):
        a = math.radians(deg)
        r1 = R * (1.06 if deg % 15 == 0 else 1.03)
        r2 = R * 1.10
        c.line([(cx + math.cos(a) * r1, cy + math.sin(a) * r1), (cx + math.cos(a) * r2, cy + math.sin(a) * r2)],
               GOLD, 0.55 if deg % 15 == 0 else 0.3, 2)
    # golden pentagon and its star, inscribed
    pts = [(cx + math.cos(math.radians(-90 + 72 * k)) * R * 0.98, cy + math.sin(math.radians(-90 + 72 * k)) * R * 0.98) for k in range(5)]
    c.line(pts + [pts[0]], GOLD, 0.35, 2)
    c.line([pts[0], pts[2], pts[4], pts[1], pts[3], pts[0]], GOLD, 0.28, 1.6)
    # cyan nodes where the figures meet
    for q in pts:
        c.circle(q[0], q[1], 9, CYAN, 0.95, 0, fill=CYAN + (230,))
    # crosshair and HUD arcs in cyan
    c.line([(cx - R * 1.25, cy), (cx - R * 1.12, cy)], CYAN, 0.7, 2)
    c.line([(cx + R * 1.12, cy), (cx + R * 1.25, cy)], CYAN, 0.7, 2)
    c.arc(cx, cy, R * 1.22, 200, 250, CYAN, 0.75, 4)
    c.arc(cx, cy, R * 1.22, 20, 70, CYAN, 0.75, 4)
    c.arc(cx, cy, R * 1.34, 120, 150, MAGENTA, 0.6, 3)
    c.glow(7, 0.9)
    return c.done()


# ---- layer 2: the arcade: round arches on columns, dark lapis, gold edge, a cyan line inside ------------------
def layer2():
    c = Canvas(DEPTHS[2])
    base = 1830
    band = Image.new("L", c.im.size, 0)
    bd = ImageDraw.Draw(band)
    arches = 5
    width = SW / arches
    # the wall: everything below base, with arched openings cut out
    def P(x, y):
        return c.p(x, y)
    bd.rectangle((0, P(0, base)[1], c.im.width, c.im.height), fill=255)
    for i in range(arches):
        x0 = i * width + 26
        x1 = (i + 1) * width - 26
        mid = (x0 + x1) / 2
        r = (x1 - x0) / 2
        top_y = base + 150
        (ax0, ay0), (ax1, ay1) = P(x0, top_y), P(x1, 2500)
        bd.rectangle((ax0, P(0, top_y + r)[1] * 0 + ay0 + (ay1 - ay0) * 0, ax1, ay1), fill=0)
        # semicircular head
        hx, hy = P(mid, top_y)
        rr = to_layer(DEPTHS[2], 0, 0, r)[2]
        bd.ellipse((hx - rr, hy - rr, hx + rr, hy + rr), fill=0)
        bd.rectangle((hx - rr, hy, hx + rr, ay1), fill=0)
    wall = Image.new("RGBA", c.im.size, (13, 14, 36, 255))
    # a little marble grain
    grain = (rng.normal(0, 1, (c.im.height // 8, c.im.width // 8)) * 6 + 6).clip(0, 30).astype(np.uint8)
    g = Image.fromarray(grain).resize(c.im.size, Image.BICUBIC).filter(ImageFilter.GaussianBlur(6))
    wall = Image.merge("RGBA", (ImageChops.add(wall.split()[0], g), ImageChops.add(wall.split()[1], g),
                                ImageChops.add(wall.split()[2], g), Image.new("L", c.im.size, 255)))
    c.im.paste(wall, (0, 0), band)
    c.d = ImageDraw.Draw(c.im, "RGBA")
    # the cornice
    c.line([(-50, base), (1130, base)], GOLD, 0.8, 4)
    c.line([(-50, base + 16), (1130, base + 16)], GOLD, 0.35, 2)
    # arch edges: gold outline and a cyan inner line
    for i in range(arches):
        x0, x1 = i * width + 26, (i + 1) * width - 26
        mid, r = (x0 + x1) / 2, (x1 - x0) / 2
        top_y = base + 150
        for inset, col, a, w in [(0, GOLD, 0.85, 3.5), (14, CYAN, 0.7, 2.5)]:
            rr = r - inset
            c.arc(mid, top_y, rr, 180, 360, col, a, w)
            c.line([(mid - rr, top_y), (mid - rr, 2500)], col, a, w)
            c.line([(mid + rr, top_y), (mid + rr, 2500)], col, a, w)
        # keystone
        c.circle(mid, top_y - r, 7, GOLD_HI, 0.9, 0, fill=GOLD_HI + (220,))
    c.glow(5, 0.7)
    return c.done()


# ---- layer 3: the owl of Minerva framed by laurel -----------------------------------------------------------
def layer3():
    c = Canvas(DEPTHS[3])
    cx, cy = 540, 1010
    # laurel wreath: a stem along an arc each side, from the bottom up, a pair of pointed leaves at each node
    R = 330

    def leaf(x, y, ang, L, W):
        pts = []
        for u in [i / 12 for i in range(13)]:
            pts.append((u * L, W * math.sin(math.pi * u) * (1 - 0.2 * u)))
        for u in [i / 12 for i in range(12, -1, -1)]:
            pts.append((u * L, -W * math.sin(math.pi * u) * (1 - 0.2 * u)))
        poly = [(x + px * math.cos(ang) - py * math.sin(ang), y + px * math.sin(ang) + py * math.cos(ang)) for px, py in pts]
        c.d.polygon([c.p(*q) for q in poly], fill=GOLD + (215,), outline=GOLD_HI + (255,))
        c.line([(x, y), (x + L * 0.95 * math.cos(ang), y + L * 0.95 * math.sin(ang))], GOLD_HI, 0.8, 1.5)

    for side in (-1, 1):
        steps = 14
        stem = []
        for k in range(steps + 1):
            t = k / steps
            phi = math.radians(95 + side * -1 * t * 165) if side == 1 else math.radians(85 + t * 165)
            stem.append((cx + math.cos(phi) * R, cy + math.sin(phi) * R))
        c.line(stem, GOLD, 0.9, 5)
        for k in range(1, steps + 1):
            x, y = stem[k]
            px, py = stem[k - 1]
            tang = math.atan2(y - py, x - px)
            size = 1.0 - 0.35 * (k / steps)
            for off in (-0.62, 0.62):
                leaf(x, y, tang + off, 70 * size, 19 * size)
    # the owl: brow arcs, eyes, beak, breast feathers
    for sx in (-1, 1):
        ex = cx + sx * 118
        c.circle(ex, cy - 30, 100, GOLD, 0.95, 5)
        c.circle(ex, cy - 30, 78, GOLD, 0.45, 2)
        c.circle(ex, cy - 30, 52, CYAN, 1.0, 0, fill=CYAN + (255,))
        c.circle(ex, cy - 30, 22, (6, 8, 20), 1.0, 0, fill=(6, 8, 20, 255))
        c.circle(ex + sx * 10, cy - 42, 8, (255, 255, 255), 1.0, 0, fill=(255, 255, 255, 230))
        # brow: an arc sweeping up and out into an ear tuft
        c.arc(ex, cy - 30, 122, 200 if sx < 0 else 270, 270 if sx < 0 else 340, GOLD_HI, 0.9, 6)
        tuft_x = cx + sx * 215
        c.line([(cx + sx * 150, cy - 140), (tuft_x, cy - 235), (cx + sx * 90, cy - 165)], GOLD_HI, 0.9, 5)
    # beak
    c.line([(cx - 26, cy + 10), (cx, cy + 84), (cx + 26, cy + 10), (cx - 26, cy + 10)], GOLD_HI, 0.95, 5)
    c.line([(cx, cy - 25), (cx, cy + 10)], GOLD, 0.8, 4)
    # breast: rows of chevrons
    for row in range(6):
        y = cy + 120 + row * 44
        half = 60 + row * 28
        pts = [(cx - half, y), (cx, y + 36), (cx + half, y)]
        c.line(pts, GOLD, 0.8 - row * 0.09, 4)
    c.glow(6, 0.9)
    return c.done()


# ---- layer 4: in front: glitch bars, gold motes ---------------------------------------------------------------
def layer4():
    c = Canvas(DEPTHS[4])
    for y, w, off, col in [(430, 520, 40, CYAN), (433, 520, 52, MAGENTA), (1370, 360, 0, CYAN), (1373, 360, -12, MAGENTA),
                           (2050, 640, 90, CYAN), (2054, 640, 102, MAGENTA), (780, 200, 380, GOLD)]:
        x0 = -60 + off + rng.uniform(0, 60)
        c.line([(x0, y), (x0 + w, y)], col, 0.55, 4)
    # HUD corner brackets
    for sx, sy in [(0, 0), (1, 0), (0, 1), (1, 1)]:
        x = 38 if sx == 0 else 1042
        y = 150 if sy == 0 else 2250
        dx, dy = (1 if sx == 0 else -1) * 60, (1 if sy == 0 else -1) * 60
        c.line([(x, y + dy), (x, y), (x + dx, y)], GOLD, 0.6, 3)
    c.glow(5, 0.8)
    # soft gold motes
    motes = Canvas(DEPTHS[4])
    for _ in range(26):
        x, y, r = rng.uniform(0, SW), rng.uniform(100, SH - 100), rng.uniform(5, 14)
        motes.circle(x, y, r, GOLD_HI, 1, 0, fill=GOLD_HI + (int(rng.uniform(50, 130)),))
    motes.im = motes.im.filter(ImageFilter.GaussianBlur(to_layer(DEPTHS[4], 0, 0, 4)[2]))
    c.im = Image.alpha_composite(c.im, motes.im)
    return c.done()


def flatten(layers, w, h, scale=SCALE):
    out = Image.new("RGBA", (w, h), (0, 0, 0, 255))
    for im, depth in layers:
        m = math.ceil(RANGE * abs(depth) * scale)
        bw, bh = w + 2 * m, h + 2 * m
        side = max(bw, bh)
        big = im.convert("RGBA").resize((side, side), Image.LANCZOS)
        big = big.crop(((side - bw) // 2, (side - bh) // 2, (side - bw) // 2 + bw, (side - bh) // 2 + bh))
        out.alpha_composite(big, (-m, -m))
    return out.convert("RGB")


def main():
    os.makedirs(OUT, exist_ok=True)
    l0 = layer0()
    layers = [l0, layer1(), layer2(), layer3(), layer4()]
    l0.save(os.path.join(OUT, "layer0.jpg"), quality=90)
    for i in range(1, 5):
        layers[i].save(os.path.join(OUT, f"layer{i}.png"), optimize=True)
    json.dump({"range": RANGE, "tilt": TILT, "layers": [
        {"file": "layer0.jpg", "depth": DEPTHS[0]},
        *[{"file": f"layer{i}.png", "depth": DEPTHS[i]} for i in range(1, 5)]]},
        open(os.path.join(OUT, "parallax.json"), "w"), indent=2)
    wall = flatten(list(zip(layers, DEPTHS)), SW, SH)
    wall.save(os.path.join(OUT, "wallpaper.jpg"), quality=92)
    wall.crop((0, 500, SW, 500 + SW)).resize((512, 512), Image.LANCZOS).save(os.path.join(OUT, "preview.jpg"), quality=88)
    print("wrote", OUT)


main()
