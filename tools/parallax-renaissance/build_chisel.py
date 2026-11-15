#!/usr/bin/python3 -I
# build_chisel.py OUTDIR [--only LOOK] [--check DIR]: the four theme-v4 wallpaper sets for the Neo launcher,
# neo-launcher/parallax/renaissance-{marble,sea,forest,plum}/. A faithful port of the design mockup's JS
# (marble(), sand(), the grain specks, grooves() and the lighting in draw()): same LCG (rng()), same noise, same
# seeds (7 for marble/sea, 11 for forest/plum, grooves 5), same constants.
#
# Per set:
#   base.jpg        the stone / sand without any marks, 1080x2400 (the launcher's one layer)
#   wallpaper.jpg   the still for the lock screen and GNOME: base + the chisel marks and the soft light glow at the
#                   mockup's default light (tx .3, ty -.3)
#   chisel-lit-NN.png, chisel-dark-NN.png
#                   the grooves, pre-rendered into BUCKETS orientation buckets by their normal: each PNG holds the
#                   coverage (8-bit grey, w already in it) of the strokes of that bucket, the lit wall and the
#                   shadow wall apart, for the two corner regions only (top right over bottom left, half resolution)
#   parallax.json   the layer plus the "chisel" block (see neo-launcher/src/parallax.c): the launcher tints each bucket
#                   with alpha = the mockup's formula for the current light, so the tilt moves one light value
# The mockup draws the grooves on a 360x760 canvas; here that design space is mapped onto 1080x2400.
# Needs python3-pil and python3-numpy. Deterministic.
import json
import math
import os
import sys

import numpy as np
from PIL import Image

PW, PH = 1080, 2400          # the phone
DW, DH = 360, 760            # the mockup's design space (its canvas is the phone scaled to this)
SX, SY = PW / DW, PH / DH
S = PW / DW                  # isotropic scale for sizes (stroke width, length)
BUCKETS = 10                 # orientation buckets over the half circle the normals can point to
HALF = 2                     # chisel textures are stored at 1/HALF of the pixel size
LIGHT0 = (0.3, -0.3)         # the mockup's default light, the still's

# look -> mockup mode, seed, palette, chisel colours (lit, dark), gains, glow
SETS = {
    "marble": dict(mode="day", seed=7, day=True),
    "sea": dict(mode="sea", seed=7, day=True),
    "forest": dict(mode="forest", seed=11, day=False),
    "plum": dict(mode="night", seed=11, day=False),
}
PAL = {
    "day": dict(a=(246, 246, 244), b=(226, 228, 233), vein=(118, 128, 146), grain=None),
    "forest": dict(a=(22, 39, 30), b=(6, 12, 9), vein=(79, 122, 92), grain=((217, 242, 166), (233, 201, 138)), gd=3),
    "night": dict(a=(34, 29, 44), b=(12, 10, 17), vein=(120, 104, 146), grain=((236, 238, 244), (233, 201, 138))),
}
LIT = {"day": (255, 255, 255), "sea": (255, 255, 255), "forest": (214, 236, 190), "night": (243, 220, 160)}
DARK = {"day": (60, 62, 74), "sea": (60, 62, 74), "forest": (0, 0, 0), "night": (0, 0, 0)}
GLOW = {True: ((255, 255, 255), 0.35), False: ((240, 215, 170), 0.10)}


# ---- the mockup's rng() and noise() ----------------------------------------------------------------------------
class Rng:
    def __init__(self, seed):
        self.s = seed & 0xFFFFFFFF

    def __call__(self):
        self.s = (self.s * 1664525 + 1013904223) & 0xFFFFFFFF
        return self.s / 4294967296

    def many(self, n):
        out = np.empty(n)
        s = self.s
        for i in range(n):
            s = (s * 1664525 + 1013904223) & 0xFFFFFFFF
            out[i] = s / 4294967296
        self.s = s
        return out


def make_noise(w, h, cell, r):
    """value noise on a (w, h) canvas: the grid draws in the mockup's order; evaluated at any canvas coordinates"""
    gw, gh = math.ceil(w / cell) + 2, math.ceil(h / cell) + 2
    g = np.array([r() for _ in range(gw * gh)], dtype=np.float32).astype(np.float64).reshape(gh, gw)

    def f(x, y):
        x = x / cell
        y = y / cell
        i, j = np.floor(x).astype(int), np.floor(y).astype(int)
        fx, fy = x - i, y - j
        sx, sy = fx * fx * (3 - 2 * fx), fy * fy * (3 - 2 * fy)
        a, b, c, d = g[j, i], g[j, i + 1], g[j + 1, i], g[j + 1, i + 1]
        return a + (b - a) * sx + (c - a + (a - b + d - c) * sx) * sy
    return f


def to_u8(a):
    return np.clip(np.rint(a), 0, 255).astype(np.uint8)


# ---- stone: smooth marble, computed on the mockup's canvas, then scaled to the phone as the mockup does -------------
def marble(mode, seed):
    p, r = PAL[mode], Rng(seed)
    n = [make_noise(DW, DH, c, r) for c in (180, 70, 26, 9)]
    k = 0.55 if mode == "day" else 0.45
    X, Y = np.meshgrid(np.arange(DW, dtype=float), np.arange(DH, dtype=float))
    f = n[0](X, Y) * .5 + n[1](X, Y) * .28 + n[2](X, Y) * .10 + n[3](X, Y) * .03
    v1 = (1 - np.abs(np.sin((X * .55 + Y * .85) / DW * 7 + f * 9))) ** 18
    v2 = (1 - np.abs(np.sin((X * .9 - Y * .3) / DW * 4 + f * 13 + 2))) ** 40 * .6
    t = np.minimum(1, Y / DH * .7 + f * .5)
    v = np.minimum(1, (v1 * .55 + v2) * k)
    out = np.empty((DH, DW, 3))
    for ch in range(3):
        base = p["a"][ch] + (p["b"][ch] - p["a"][ch]) * t
        out[..., ch] = base + (p["vein"][ch] - base) * v
    big = np.asarray(Image.fromarray(to_u8(out)).resize((PW, PH), Image.BILINEAR)).astype(np.float64)
    if p["grain"]:
        specks(big, p, r)
    return to_u8(big)


def specks(img, p, r):
    """the grain specks ("fireflies"): the mockup's arcs, drawn anti-aliased at the phone's size"""
    for _ in range(math.ceil(DW * DH / 900 / (p.get("gd") or 1))):     # the JS loop: i < a float bound
        g = p["grain"][0 if r() < .8 else 1]
        alpha = round(r() * .35 + .05, 2)
        cx, cy, rad = r() * DW * SX, r() * DH * SY, (r() * 1.4 + .3) * S
        x0, x1 = max(0, int(cx - rad - 2)), min(PW, int(cx + rad + 3))
        y0, y1 = max(0, int(cy - rad - 2)), min(PH, int(cy + rad + 3))
        if x1 <= x0 or y1 <= y0:
            continue
        X, Y = np.meshgrid(np.arange(x0, x1) + .5, np.arange(y0, y1) + .5)
        a = np.clip(rad + .5 - np.hypot(X - cx, Y - cy), 0, 1)[..., None] * alpha
        img[y0:y1, x0:x1] = img[y0:y1, x0:x1] * (1 - a) + np.array(g) * a


# ---- Marino: light sand, ordered 4x4 dither, wind ripples, sky blue washing in from the top ----------------------
B4 = (np.array([0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5]) + .5) / 16


def sand(seed):
    r = Rng(seed)
    W, H = DW, DH
    n = [make_noise(W, H, c, r) for c in (160, 40, 7)]
    SA, SB, SKY, DOT, LITC = (np.array(c, float) for c in
                              ((244, 237, 223), (232, 222, 203), (172, 212, 238), (205, 190, 162), (252, 249, 242)))
    X, Y = np.meshgrid(np.arange(W, dtype=float), np.arange(H, dtype=float))
    f = n[0](X, Y) * .6 + n[1](X, Y) * .3 + n[2](X, Y) * .1
    ripple = np.sin((Y + f * 60) / H * 46 + X / W * 3) * .5 + .5
    sky = np.clip((.42 - Y / H + f * .18) / .42, 0, 1)
    skyT = (sky * sky * (3 - 2 * sky) * .85)[..., None]
    t = np.minimum(1, Y / H * .6 + f * .4)[..., None]
    b = B4[((Y.astype(int) & 3) * 4 + (X.astype(int) & 3))]
    grain = f * .55 + ripple * .25 + r.many(W * H).reshape(H, W) * .35
    v = SA + (SB - SA) * t
    dot, lit = (grain > b + .62)[..., None], (grain < b - .38)[..., None]
    v = np.where(dot, v + (DOT - v) * .55, np.where(lit, v + (LITC - v) * .6, v))
    v = v + (SKY - v) * skyT
    small = Image.fromarray(to_u8(v))                       # 360x760, as in the mockup, then scaled to the phone
    return np.asarray(small.resize((PW, PH), Image.BILINEAR))


# ---- the chisel grooves (the mockup's grooves(), in its 360x760 design space) ------------------------------------
def grooves(W, H, seed):
    r, out, R = Rng(seed), [], min(W, H) * .78
    for cx, cy in ((W, 0), (0, H)):
        for _ in range(120):
            a = r() * math.pi / 2
            d = r() ** .8 * R
            px = cx + (-1 if cx else 1) * math.cos(a) * d
            py = cy + (-1 if cy else 1) * math.sin(a) * d
            ang = r() * math.pi
            ln = 10 + r() * 16
            cnt = 4 + int(r() * 6)
            w = max(0, 1 - math.hypot(px - cx, py - cy) / R) ** 1.6
            if w < .03:
                continue
            nx, ny = -math.sin(ang), math.cos(ang)
            for k in range(cnt):
                o = (k - cnt / 2) * 3.2
                mx = px + nx * o + (r() - .5) * 2
                my = py + ny * o + (r() - .5) * 2
                l = ln * (.7 + r() * .5)
                out.append(dict(x0=mx - math.cos(ang) * l / 2, y0=my - math.sin(ang) * l / 2,
                                x1=mx + math.cos(ang) * l / 2, y1=my + math.sin(ang) * l / 2,
                                nx=nx, ny=ny, w=w * (.7 + r() * .3), corner=1 if cy else 0))
    return out


def bucket_angle(i):
    """normals are (-sin a, cos a): nx <= 0, so they cover 90..270 degrees (y down), as many buckets as BUCKETS"""
    return 90 + (i + .5) * 180 / BUCKETS


def bucket_of(g):
    deg = math.degrees(math.atan2(g["ny"], g["nx"])) % 360
    return min(BUCKETS - 1, max(0, int((deg - 90) / (180 / BUCKETS))))


def stroke_px(g, sign):
    """a stroke's segment in phone pixels, the wall offset by sign * normal (1 design px = S px)"""
    cx, cy = (g["x0"] + g["x1"]) / 2 * SX + sign * g["nx"] * S, (g["y0"] + g["y1"]) / 2 * SY + sign * g["ny"] * S
    hx, hy = (g["x1"] - g["x0"]) / 2 * S, (g["y1"] - g["y0"]) / 2 * S
    return cx - hx, cy - hy, cx + hx, cy + hy


def tile_size(G):
    """the corner regions' size: the strokes' reach from their corner, over both corners, even pixels"""
    cw = ch = 0
    for g in G:
        for sign in (1, -1):
            xa, ya, xb, yb = stroke_px(g, sign)
            x0, x1, y0, y1 = min(xa, xb), max(xa, xb), min(ya, yb), max(ya, yb)
            if g["corner"] == 0:
                cw, ch = max(cw, PW - x0), max(ch, y1)
            else:
                cw, ch = max(cw, x1), max(ch, PH - y0)
    pad = 4 + S
    return int(math.ceil((cw + pad) / 4) * 4), int(math.ceil((ch + pad) / 4) * 4)


def render_tiles(G, cw, ch, sign):
    """BUCKETS tiles of (ch/HALF*2, cw/HALF): top right over bottom left, coverage with the strokes' w baked in"""
    tw, th = cw // HALF, ch // HALF
    tiles = np.zeros((BUCKETS, 2 * th, tw), np.float32)
    hw = 1.7 * S / 2 / HALF
    for g in G:
        xa, ya, xb, yb = stroke_px(g, sign)
        # tile coordinates: the corner region's origin is (PW - cw, 0) for the top right, (0, PH - ch) for the other
        ox, oy = (PW - cw, 0) if g["corner"] == 0 else (0, PH - ch)
        ax, ay, bx, by = (xa - ox) / HALF, (ya - oy) / HALF, (xb - ox) / HALF, (yb - oy) / HALF
        row = g["corner"] * th
        x0, x1 = max(0, int(min(ax, bx) - hw - 2)), min(tw, int(max(ax, bx) + hw + 3))
        y0, y1 = max(0, int(min(ay, by) - hw - 2)), min(th, int(max(ay, by) + hw + 3))
        if x1 <= x0 or y1 <= y0:
            continue
        X, Y = np.meshgrid(np.arange(x0, x1) + .5, np.arange(y0, y1) + .5)
        dx, dy = bx - ax, by - ay
        t = np.clip(((X - ax) * dx + (Y - ay) * dy) / (dx * dx + dy * dy), 0, 1)
        dist = np.hypot(X - (ax + t * dx), Y - (ay + t * dy))
        cov = np.clip(hw + .5 - dist, 0, 1) * g["w"]
        sub = tiles[bucket_of(g), row + y0:row + y1, x0:x1]
        sub[:] = 1 - (1 - sub) * (1 - cov)               # source-over of the strokes' coverage
    return tiles


# ---- the lighting: the mockup's draw(), on the buckets ---------------------------------------------------------
def light_factors(tx, ty, day):
    ln = math.hypot(tx, ty) or 1
    st = min(1, .35 + ln * .9)
    out = []
    for i in range(BUCKETS):
        th = math.radians(bucket_angle(i))
        dt = (math.cos(th) * tx + math.sin(th) * ty) / ln
        out.append((max(0, dt) * st * (1 if day else .9),
                    (.25 + max(0, -dt) * .6) * st * (.6 if day else .9)))
    return out


def upscale(tile, cw, ch):
    """one bucket's stacked tile -> two full-size corner coverages (float 0..1)"""
    im = Image.fromarray(to_u8(tile * 255)).resize((cw, 2 * ch), Image.BILINEAR)
    a = np.asarray(im, np.float32) / 255
    return a[:ch], a[ch:]


def compose(base, cfg, lit, dark, tx, ty):
    """base (PH, PW, 3) uint8 + chisel marks + glow at the light (tx, ty), uint8"""
    img = base.astype(np.float32)
    cw, ch = cfg["cw"], cfg["ch"]
    fac = light_factors(tx, ty, cfg["day"])
    for i in range(BUCKETS):
        for tiles, col, k in ((lit, cfg["lit"], 0), (dark, cfg["dark"], 1)):
            if fac[i][k] <= 0:
                continue
            tr, bl = upscale(tiles[i], cw, ch)
            for cov, ys, xs in ((tr, slice(0, ch), slice(PW - cw, PW)), (bl, slice(PH - ch, PH), slice(0, cw))):
                a = (cov * fac[i][k])[..., None]
                img[ys, xs] = img[ys, xs] * (1 - a) + np.array(col, np.float32) * a
    gcol, galpha = GLOW[cfg["day"]]
    sx, sy, rad = PW * (.5 + tx * .45), PH * (.35 + ty * .35), PW * .9
    X, Y = np.meshgrid(np.arange(PW) + .5, np.arange(PH) + .5)
    a = (np.clip(1 - np.hypot(X - sx, Y - sy) / rad, 0, 1) * galpha)[..., None]
    img = img * (1 - a) + np.array(gcol, np.float32) * a
    return to_u8(img)


def write_set(out_dir, name, cfg_in):
    mode = cfg_in["mode"]
    base = sand(cfg_in["seed"]) if mode == "sea" else marble(mode, cfg_in["seed"])
    G = grooves(DW, DH, 5)
    cw, ch = tile_size(G)
    lit, dark = render_tiles(G, cw, ch, 1), render_tiles(G, cw, ch, -1)
    os.makedirs(out_dir, exist_ok=True)
    Image.fromarray(base).save(os.path.join(out_dir, "base.jpg"), quality=88, optimize=True)
    buckets = []
    for i in range(BUCKETS):
        for kind, tiles in (("lit", lit), ("dark", dark)):
            Image.fromarray(to_u8(tiles[i] * 255), "L").save(os.path.join(out_dir, f"chisel-{kind}-{i:02d}.png"),
                                                              optimize=True)
        buckets.append({"angle": round(bucket_angle(i), 3), "lit": f"chisel-lit-{i:02d}.png",
                        "dark": f"chisel-dark-{i:02d}.png"})
    cfg = dict(cw=cw, ch=ch, day=cfg_in["day"], lit=LIT[mode], dark=DARK[mode])
    still = compose(base, cfg, lit, dark, *LIGHT0)
    Image.fromarray(still).save(os.path.join(out_dir, "wallpaper.jpg"), quality=88, optimize=True)
    gcol, galpha = GLOW[cfg["day"]]
    day = cfg["day"]
    meta = {
        "range": 60, "tilt": 0.2,
        "layers": [{"file": "base.jpg", "depth": 0}],
        "chisel": {
            "design": [PW, PH], "tile": [cw, ch], "scale": HALF,
            "lit": list(LIT[mode]), "dark": list(DARK[mode]),
            "lit_gain": 1.0 if day else 0.9, "dark_gain": 0.6 if day else 0.9,
            "glow": {"color": list(gcol), "alpha": galpha, "radius": 0.9, "x": [0.5, 0.45], "y": [0.35, 0.35]},
            "buckets": buckets,
        },
    }
    with open(os.path.join(out_dir, "parallax.json"), "w") as fh:
        json.dump(meta, fh, indent=2)
        fh.write("\n")
    return base, cfg, lit, dark


def main():
    args = sys.argv[1:]
    only = args[args.index("--only") + 1] if "--only" in args else None
    check = args[args.index("--check") + 1] if "--check" in args else None
    out = args[0]
    for name, s in SETS.items():
        if only and only != name:
            continue
        base, cfg, lit, dark = write_set(os.path.join(out, "renaissance-" + name), name, s)
        if check:                                           # composites at other lights, to look at
            os.makedirs(check, exist_ok=True)
            for tag, (tx, ty) in (("tr", (.9, -.9)), ("bl", (-.9, .9)), ("centre", (0, 0)), ("left", (-.8, 0))):
                Image.fromarray(compose(base, cfg, lit, dark, tx, ty)).resize((PW // 2, PH // 2), Image.LANCZOS) \
                    .save(os.path.join(check, f"{name}-{tag}.png"))
        print("wrote", name)


if __name__ == "__main__":
    main()
