#!/usr/bin/python3 -I
# make-tiles.py OUTDIR --mode day|night [--glyphs DIR] [--owl PNG]
#
# The two framed Renaissance icon packs of the theme (neo-launcher/iconpacks/renaissance-{day,night}): the icon of the
# mock-up (in_models/in_gemini/ICON_COMPONENTS.jpeg, MINERVA_AGENT_CORE.jpeg) assembled on a square base — a neon halo
# outside a bevelled platinum frame with diamond points at the edge centres, a marble plate with gold acanthus corners
# and gear clusters, a marble/platinum disc with sacred-geometry wires, and the app's fine-line glyph tinted toward its
# neon. Day is `milk` (a milky, slightly see-through plate, ink glyph), night `smoke` (a dark smoked plate, pale-gold
# glyph and full neon); Minerva's own icon (`yesman`) is the owl in a toothed gear ring (`agent`).
# Glyphs: Material Symbols Outlined, weight 200 (Google, Apache-2.0; glyphs/LICENSE, fetched by fetch-glyphs.sh).
# Deterministic (every texture is seeded from the icon's name). Needs python3-gi with Rsvg 2.0, python3-cairo,
# python3-pil and python3-numpy.
import argparse
import json
import math
import os
import random

import cairo
import gi
import numpy as np
gi.require_version("Rsvg", "2.0")
from gi.repository import Rsvg
from PIL import Image, ImageChops, ImageFilter

PX, SS = 256, 4
N = PX * SS
TAU = math.tau
M = 0.07  # frame margin (room for the halo)

PAL = {
    "granite": dict(plate=("#2c2636", "#100e16"), vein="#7a6a8c", disc=("#2a2830", "#0f0e13"), dvein="#5a5866",
                    glyph="#f3dc9f", gold=("#f3d79b", "#9c7432"), plat=("#f2f4f7", "#6d727c"), wire=0.55, gear_a=0.6),
    "marble": dict(plate=("#f6f5f2", "#dcd9d3"), vein="#8a8d95", disc=("#fdfcfa", "#e8e5df"), dvein="#9a9da5",
                   glyph="#2a2b2e", gold=("#ead19a", "#a07a3a"), plat=("#ffffff", "#8a8f99"), wire=0.45, gear_a=0.5),
    "sea": dict(plate=("#f5f7f9", "#d9dfe5"), vein="#7f93a6", disc=("#fcfdfe", "#e6ebf0"), dvein="#8fa2b3",
                glyph="#2a2b2e", gold=("#ead19a", "#a07a3a"), plat=("#ffffff", "#8a8f99"), wire=0.45, gear_a=0.5),
    "forest": dict(plate=("#1b2e23", "#060c09"), vein="#4f7a5c", disc=("#1c2a22", "#09100c"), dvein="#3e5a48",
                   glyph="#efe0b0", gold=("#f3d79b", "#9c7432"), plat=("#f2f4f7", "#6d727c"), wire=0.5, gear_a=0.55),
}
# milk (day): the marble icon on a milky, slightly see-through plate; smoke (night): the same build on a dark
# semi-transparent plate, glyph in the night ink (light gold + full neon) so it reads on the dark
PAL["milk"] = dict(PAL["marble"], plate=("#fbfaf7", "#ecebe7"), vein="#b4b6bc")
PAL["smoke"] = dict(PAL["granite"], plate=("#16141b", "#040306"), vein="#3a3642", gear_a=0.45)
PLATE_ALPHA = {"milk": 0.84, "smoke": 0.66}
# the "verde marmo" inlay ring of the day disc (verde Alpi: dark green, pale veins)
INLAY = {"marble": ("#2f6b4f", "#0f3324", "#d6e8db"), "sea": ("#5aaee0", "#1d5f8c", "#e3f3fc")}
LIGHT = {"marble", "sea", "milk"}


def mix(a, b, t):
    ca, cb = rgb(a), rgb(b)
    return "#%02x%02x%02x" % tuple(int(255 * (x + (y - x) * t)) for x, y in zip(ca, cb))


def rgb(h, a=None):
    h = h.lstrip("#")
    c = tuple(int(h[i:i + 2], 16) / 255 for i in (0, 2, 4))
    return c if a is None else c + (a,)


def surface():
    s = cairo.ImageSurface(cairo.FORMAT_ARGB32, N, N)
    return s, cairo.Context(s)


def to_pil(s):
    s.flush()
    return Image.frombuffer("RGBA", (N, N), bytes(s.get_data()), "raw", "BGRa", 0, 1).copy()


def rrect(cr, inset, r):
    x0, y0, x1, y1 = inset, inset, N - inset, N - inset
    cr.new_sub_path()
    cr.arc(x1 - r, y0 + r, r, -TAU / 4, 0)
    cr.arc(x1 - r, y1 - r, r, 0, TAU / 4)
    cr.arc(x0 + r, y1 - r, r, TAU / 4, TAU / 2)
    cr.arc(x0 + r, y0 + r, r, TAU / 2, 3 * TAU / 4)
    cr.close_path()


def lin(c1, c2, x0=0, y0=0, x1=N, y1=N):
    g = cairo.LinearGradient(x0, y0, x1, y1)
    g.add_color_stop_rgb(0, *rgb(c1))
    g.add_color_stop_rgb(1, *rgb(c2))
    return g


# ---- textures

def vnoise(shape, cell, rng):
    gh, gw = shape[0] // cell + 2, shape[1] // cell + 2
    g = rng.random((gh, gw))
    y = np.arange(shape[0]) / cell
    x = np.arange(shape[1]) / cell
    y0, x0 = y.astype(int), x.astype(int)
    fy, fx = (y - y0)[:, None], (x - x0)[None, :]
    fy, fx = fy * fy * (3 - 2 * fy), fx * fx * (3 - 2 * fx)
    a = g[y0][:, x0]; b = g[y0][:, x0 + 1]; c = g[y0 + 1][:, x0]; d = g[y0 + 1][:, x0 + 1]
    return (a * (1 - fx) + b * fx) * (1 - fy) + (c * (1 - fx) + d * fx) * fy


def fbm(rng, base, octaves):
    out, amp, tot = np.zeros((N, N)), 1.0, 0.0
    for o in range(octaves):
        out += amp * vnoise((N, N), max(2, base >> o), rng)
        tot += amp
        amp *= 0.5
    return out / tot


def guide_field(kind, xx, yy):
    """the 'marble pattern guides': a symmetric scalar field whose level lines the veins follow"""
    x, y = xx - 0.5, yy - 0.5
    if kind == 0:  # book-matched chevrons (mirrored about the vertical axis)
        return np.abs(x) * 1.5 + 1.2 * y + 0.9 * y * y
    if kind == 1:  # concentric squircles
        return (x ** 4 + y ** 4) ** 0.25 * 2.6
    if kind == 2:  # four-fold hyperbolic flow from the corners
        return x * y * 9.0
    return (np.abs(x + y) * 1.4 + 0.8 * (x - y) ** 2) * 1.3  # diagonal book-match


def marble(c1, c2, vein_col, seed, strength=0.5, stars=False, guide=None):
    rng = np.random.default_rng(seed)
    yy, xx = np.mgrid[0:N, 0:N] / N
    a, b = np.array(rgb(c1)), np.array(rgb(c2))
    col = a + (b - a) * (xx * 0.45 + yy * 0.55)[..., None]
    vc = np.array(rgb(vein_col))
    gf = guide_field(guide, xx, yy) if guide is not None else None
    for k, (fx, fy, amp, width, w) in enumerate(((1.1, 1.7, 4.0, 0.05, 1.0), (-1.6, 0.9, 3.0, 0.035, 0.55))):
        warp = fbm(rng, 420, 3)
        if gf is not None:  # veins along the guide's level lines, only a little warp so the flow stays deliberate
            v = np.sin(gf * math.pi * (3.0 + 2.2 * k) + k * 1.3 + warp * amp * 0.28)
        else:
            v = np.sin((xx * fx + yy * fy) * math.pi * 1.6 + warp * amp)
        fade = np.clip(fbm(rng, 300, 2) * 1.8 - 0.4, 0, 1)  # veins come and go
        vein = (np.exp(-(v / width) ** 2) * 0.9 + np.exp(-(v / (width * 5)) ** 2) * 0.25) * fade * w * strength
        col = col * (1 - vein[..., None]) + vc * vein[..., None]
    col += (fbm(rng, 48, 3)[..., None] - 0.5) * 0.035
    if stars:
        sp = rng.random((N, N))
        col += ((sp > 0.9993) * 0.7)[..., None]
    im = Image.fromarray((np.clip(col, 0, 1) * 255).astype(np.uint8), "RGB").convert("RGBA")
    return im.filter(ImageFilter.GaussianBlur(1.2))


def clip_to(im, path_fn):
    s, cr = surface()
    path_fn(cr)
    cr.set_source_rgb(1, 1, 1)
    cr.fill()
    out = Image.new("RGBA", (N, N))
    out.paste(im, (0, 0), to_pil(s).getchannel("A"))
    return out


def glow(line, radius, times=2):
    g = line.filter(ImageFilter.GaussianBlur(radius))
    out = Image.new("RGBA", (N, N))
    for _ in range(times):
        out.alpha_composite(g)
    return out


# ---- components

def gear(cr, cx, cy, r, teeth, rot, spokes=5):
    cr.save()
    cr.translate(cx, cy)
    cr.rotate(rot)
    rr = r * 0.84
    cr.new_path()
    for i in range(teeth):
        a0, w = TAU * i / teeth, TAU / teeth
        for rad, ang in ((rr, a0), (r, a0 + w * 0.12), (r, a0 + w * 0.42), (rr, a0 + w * 0.54)):
            cr.line_to(rad * math.cos(ang), rad * math.sin(ang))
    cr.close_path()
    ri = r * 0.66
    for k in range(spokes):
        a0, a1 = TAU * k / spokes + 0.16, TAU * (k + 1) / spokes - 0.16
        cr.new_sub_path()
        cr.arc(0, 0, ri, a0, a1)
        cr.arc_negative(0, 0, r * 0.36, a1 - 0.08, a0 + 0.08)
        cr.close_path()
    cr.new_sub_path()
    cr.arc(0, 0, r * 0.1, 0, TAU)
    cr.restore()


def gears_layer(pal, seed):
    s, cr = surface()
    rnd = random.Random(seed)
    cr.set_fill_rule(cairo.FILL_RULE_EVEN_ODD)
    spots = [(0.25, 0.75, 0.16, 14), (0.42, 0.85, 0.08, 9), (0.76, 0.25, 0.14, 12), (0.86, 0.42, 0.065, 8),
             (0.77, 0.77, 0.10, 10), (0.24, 0.28, 0.075, 9)]
    for i, (fx, fy, fr, t) in enumerate(spots):
        gear(cr, fx * N, fy * N, fr * N, t, rnd.random() * TAU, spokes=rnd.choice((4, 5, 6)))
        cols = pal["gold"] if i % 3 != 2 else pal["plat"]
        cr.set_source(lin(*cols, 0, fy * N - fr * N, 0, fy * N + fr * N))
        cr.fill_preserve()
        cr.set_source_rgba(0, 0, 0, 0.5)
        cr.set_line_width(N * 0.004)
        cr.stroke()
    im = to_pil(s)
    im.putalpha(im.getchannel("A").point(lambda a: int(a * pal["gear_a"])))
    return im


def bez(p0, p1, p2, p3, n=48):
    for i in range(n + 1):
        t = i / n
        u = 1 - t
        yield (u ** 3 * p0[0] + 3 * u * u * t * p1[0] + 3 * u * t * t * p2[0] + t ** 3 * p3[0],
               u ** 3 * p0[1] + 3 * u * u * t * p1[1] + 3 * u * t * t * p2[1] + t ** 3 * p3[1])


def taper(cr, pts, w0, w1):
    pts = list(pts)
    for i in range(len(pts) - 1):
        t = i / (len(pts) - 1)
        cr.set_line_width(w0 + (w1 - w0) * t)
        cr.move_to(*pts[i])
        cr.line_to(*pts[i + 1])
        cr.stroke()


def volute(cr, cx, cy, r0, a0, turns, w0, w1):
    pts = []
    for i in range(61):
        t = i / 60
        a = a0 + turns * TAU * t
        r = r0 * (1 - 0.8 * t)
        pts.append((cx + r * math.cos(a), cy + r * math.sin(a)))
    taper(cr, pts, w0, w1)
    cr.arc(*pts[-1], w1 * 1.3, 0, TAU)
    cr.fill()
    return pts[0]


def half_corner(cr, s):
    """half of a corner ornament: a stem down the left edge ending in a volute, an S-scroll into the plate"""
    taper(cr, bez((0.07 * s, 0.10 * s), (0.03 * s, 0.35 * s), (0.04 * s, 0.55 * s), (0.10 * s, 0.70 * s)),
          0.07 * s, 0.035 * s)
    volute(cr, 0.17 * s, 0.70 * s, 0.07 * s, math.pi, -0.95, 0.035 * s, 0.015 * s)
    taper(cr, bez((0.10 * s, 0.14 * s), (0.20 * s, 0.30 * s), (0.30 * s, 0.24 * s), (0.36 * s, 0.40 * s)),
          0.045 * s, 0.02 * s)
    volute(cr, 0.30 * s, 0.46 * s, 0.06 * s, -0.35, 0.9, 0.02 * s, 0.01 * s)
    cr.arc(0.07 * s, 0.86 * s, 0.025 * s, 0, TAU)
    cr.fill()


def corner(cr, s):
    cr.set_line_cap(cairo.LINE_CAP_ROUND)
    half_corner(cr, s)
    cr.save()
    cr.transform(cairo.Matrix(0, 1, 1, 0, 0, 0))  # mirror across the diagonal
    half_corner(cr, s)
    cr.restore()
    # the pointed leaf on the diagonal and its bead
    cr.move_to(0.03 * s, 0.03 * s)
    cr.curve_to(0.22 * s, 0.10 * s, 0.30 * s, 0.22 * s, 0.34 * s, 0.34 * s)
    cr.curve_to(0.22 * s, 0.30 * s, 0.10 * s, 0.22 * s, 0.03 * s, 0.03 * s)
    cr.fill()
    cr.arc(0.40 * s, 0.40 * s, 0.03 * s, 0, TAU)
    cr.fill()


def filigree_layer(pal, inset):
    s, cr = surface()
    size = N * 0.26
    for sx, sy in ((1, 1), (-1, 1), (1, -1), (-1, -1)):
        for src, d in ((rgb("#000000", 0.6), 0.005), (None, 0)):
            cr.save()
            cr.translate(N / 2 + d * N, N / 2 + d * N)
            cr.scale(sx, sy)
            cr.translate(-N / 2 + inset, -N / 2 + inset)
            if src:
                cr.set_source_rgba(*src)
            else:
                cr.set_source(lin(*pal["gold"], 0, 0, size * 0.6, size * 0.6))
            corner(cr, size)
            cr.restore()
    return to_pil(s)


def frame_path(cr):
    rrect(cr, N * M, N * 0.21)


def bevel_frame(pal):
    s, cr = surface()
    m, r = N * M, N * 0.21
    frame_path(cr)
    g = cairo.LinearGradient(0, 0, N, N)
    for t, c in ((0, pal["plat"][0]), (0.45, "#b9bdc5"), (0.6, pal["plat"][1]), (1, "#c8ccd3")):
        g.add_color_stop_rgb(t, *rgb(c))
    cr.set_source(g)
    cr.fill()
    for off, col in ((N * 0.006, rgb("#ffffff", 0.75)), (-N * 0.006, rgb("#000000", 0.4))):
        cr.save()
        frame_path(cr)
        cr.clip()
        rrect(cr, m + N * 0.012, r - N * 0.012)
        cr.translate(off, off)
        cr.set_source_rgba(*col)
        cr.set_line_width(N * 0.01)
        cr.stroke()
        cr.restore()
    for ins in (0.090, 0.098):
        rrect(cr, N * ins, r - N * (ins - M))
        cr.set_source_rgba(0, 0, 0, 0.45)
        cr.set_line_width(N * 0.0035)
        cr.stroke()
    # diamond points at the edge centres
    for ang in range(4):
        cr.save()
        cr.translate(N / 2, N / 2)
        cr.rotate(ang * TAU / 4)
        y = -N / 2 + N * (M + 0.012)
        d = N * 0.035
        cr.move_to(0, y - d * 0.2)
        cr.line_to(d, y + d * 0.8)
        cr.line_to(0, y + d * 1.8)
        cr.line_to(-d, y + d * 0.8)
        cr.close_path()
        cr.set_source(lin(*pal["gold"], 0, y, 0, y + d * 2))
        cr.fill_preserve()
        cr.set_source_rgba(0, 0, 0, 0.5)
        cr.set_line_width(N * 0.003)
        cr.stroke()
        cr.restore()
    # gilt rim around the plate
    rrect(cr, N * 0.118, r - N * 0.05)
    cr.set_source(lin(*pal["gold"]))
    cr.set_line_width(N * 0.014)
    cr.stroke()
    return to_pil(s)


def plate_path(cr):
    rrect(cr, N * 0.124, N * 0.21 - N * 0.056)


def halo(neon):
    s, cr = surface()
    rrect(cr, N * (M + 0.01), N * 0.2)
    cr.set_source_rgba(*rgb(neon), 1)
    cr.set_line_width(N * 0.03)
    cr.stroke()
    return glow(to_pil(s), N * 0.022, 2)


def wires(cr, cx, cy, R, kind):
    """platinum sacred-geometry wires on the disc face"""
    cr.set_line_width(N * 0.0035)
    cr.arc(cx, cy, R, 0, TAU)
    cr.stroke()
    if kind % 2 == 0:  # eight-point star of two squares, inner circle
        for rot in (0, TAU / 8):
            for k in range(4):
                a = rot + TAU * k / 4
                cr.line_to(cx + R * math.cos(a), cy + R * math.sin(a))
            cr.close_path()
            cr.stroke()
        cr.arc(cx, cy, R * 0.54, 0, TAU)
        cr.stroke()
    else:  # flower: six circles through the centre
        for k in range(6):
            a = TAU * k / 6
            cr.arc(cx + R * 0.5 * math.cos(a), cy + R * 0.5 * math.sin(a), R * 0.5, 0, TAU)
            cr.stroke()
    for k in range(8):
        a = TAU * k / 8
        cr.arc(cx + R * math.cos(a), cy + R * math.sin(a), N * 0.008, 0, TAU)
        cr.fill()


def disc_layer(pal, variant, seed, neon, agent):
    cx = cy = N / 2
    R = N * (0.27 if agent else 0.30)
    face = marble(*pal["disc"], pal["dvein"], seed + 7, 0.35, stars=(variant not in LIGHT))
    face = clip_to(face, lambda cr: cr.arc(cx, cy, R * 0.86, 0, TAU))
    if variant in INLAY:
        c1, c2, v = INLAY[variant]
        ring = marble(c1, c2, v, seed + 3, 0.7)
        def annulus(cr):
            cr.set_fill_rule(cairo.FILL_RULE_EVEN_ODD)
            cr.arc(cx, cy, R * 0.86, 0, TAU)
            cr.new_sub_path()
            cr.arc(cx, cy, R * 0.79, 0, TAU)
        face.alpha_composite(clip_to(ring, annulus))
    ws, wc = surface()
    wc.set_source_rgba(*rgb("#7d828c" if variant in LIGHT else "#d4d7dd"), pal["wire"] * 0.6)
    wires(wc, cx, cy, R * 0.74, seed)
    face.alpha_composite(to_pil(ws))
    s, cr = surface()
    cr.set_fill_rule(cairo.FILL_RULE_EVEN_ODD)
    if agent:  # a toothed gear ring outside the bezel
        teeth, ro = 40, R * 1.16
        for i in range(teeth):
            a0, w = TAU * i / teeth, TAU / teeth
            for rad, ang in ((R * 0.99, a0), (ro, a0 + w * 0.15), (ro, a0 + w * 0.45), (R * 0.99, a0 + w * 0.6)):
                cr.line_to(cx + rad * math.cos(ang), cy + rad * math.sin(ang))
        cr.close_path()
        cr.new_sub_path()
        cr.arc(cx, cy, R * 0.86, 0, TAU)
    else:
        cr.arc(cx, cy, R, 0, TAU)
        cr.new_sub_path()
        cr.arc(cx, cy, R * 0.86, 0, TAU)
    g = cairo.LinearGradient(cx - R, cy - R, cx + R, cy + R)
    for t, c in ((0, pal["plat"][0]), (0.55, pal["plat"][1]), (1, "#d8dbe0")):
        g.add_color_stop_rgb(t, *rgb(c))
    cr.set_source(g)
    cr.fill_preserve()
    cr.set_source_rgba(0, 0, 0, 0.5)
    cr.set_line_width(N * 0.004)
    cr.stroke()
    # gold inner rim
    cr.arc(cx, cy, R * 0.93, 0, TAU)
    cr.set_source(lin(*pal["gold"]))
    cr.set_line_width(N * 0.006)
    cr.stroke()
    out = Image.new("RGBA", (N, N))
    out.alpha_composite(face)
    out.alpha_composite(to_pil(s))
    ns, nc = surface()
    nc.arc(cx, cy, R * (1.2 if agent else 0.855), 0, TAU)
    nc.set_source_rgba(*rgb(neon), 1)
    nc.set_line_width(N * 0.006)
    nc.stroke()
    line = to_pil(ns)
    out.alpha_composite(glow(line, N * 0.012))
    out.alpha_composite(line)
    sh, sc = surface()
    sc.arc(cx + N * 0.01, cy + N * 0.018, R * (1.12 if agent else 1), 0, TAU)
    sc.set_source_rgba(0, 0, 0, 0.55)
    sc.fill()
    shadow = to_pil(sh).filter(ImageFilter.GaussianBlur(N * 0.016))
    shadow.alpha_composite(out)
    return shadow


def glyph_layer(path, pal, variant, neon):
    size = N * 0.58

    def render(colour, dx=0, dy=0):
        with open(path) as f:
            svg = f.read().replace("<svg ", '<svg fill="%s" stroke="%s" stroke-width="14" stroke-linejoin="round" ' % (colour, colour), 1)
        h = Rsvg.Handle.new_from_data(svg.encode())
        s, cr = surface()
        r = Rsvg.Rectangle()
        r.x, r.y, r.width, r.height = (N - size) / 2 + dx, (N - size) / 2 + dy, size, size
        h.render_document(cr, r)
        return to_pil(s)

    def fade(im, k):
        im.putalpha(im.getchannel("A").point(lambda a: int(a * k)))
        return im

    out = Image.new("RGBA", (N, N))
    if variant in LIGHT:  # deep ink tinted toward the neon, a soft neon aura, a chiselled white lip
        out.alpha_composite(fade(glow(render(neon), N * 0.016, 2), 0.85))
        out.alpha_composite(render("#ffffff", N * 0.003, N * 0.004))
        out.alpha_composite(render(mix(pal["glyph"], neon, 0.38)))
    else:  # pale gold tinted toward the neon, full neon glow, a dark cast shadow
        out.alpha_composite(glow(render(neon), N * 0.012, 2))
        out.alpha_composite(render("#000000", N * 0.003, N * 0.005))
        out.alpha_composite(render(mix(pal["glyph"], neon, 0.30)))
    return out


def owl_layer(owl_path, variant, neon):
    owl = Image.open(owl_path).convert("RGBA")
    w = int(N * 0.66)
    h = int(owl.height * w / owl.width)
    owl = owl.resize((w, h), Image.LANCZOS)
    out = Image.new("RGBA", (N, N))
    x, y = (N - w) // 2, int(N * 0.5 - h * 0.5)
    sh = Image.new("RGBA", (N, N))
    sh.paste((0, 0, 0, 255), (x + int(N * 0.008), y + int(N * 0.014)), owl.getchannel("A"))
    sh.putalpha(sh.getchannel("A").point(lambda a: a // 2))
    out.alpha_composite(sh.filter(ImageFilter.GaussianBlur(N * 0.01)))
    out.alpha_composite(owl, (x, y))
    return out


def centre_fade(im):
    """keep the gears to the corners: fade them out where the glyph sits"""
    m = Image.radial_gradient("L").resize((N, N))  # 0 centre .. 255 edge
    m = m.point(lambda v: max(0, min(255, int((v - 70) * 2.2))))
    im.putalpha(Image.composite(im.getchannel("A"), Image.new("L", (N, N), 0), m))
    return im


def inlay_layer(variant, seed):
    if variant not in INLAY:
        return Image.new("RGBA", (N, N))
    c1, c2, v = INLAY[variant]
    ring = marble(c1, c2, v, seed + 3, 0.7)
    def band(cr):
        cr.set_fill_rule(cairo.FILL_RULE_EVEN_ODD)
        rrect(cr, N * 0.150, N * 0.115)
        rrect(cr, N * 0.166, N * 0.100)
    out = clip_to(ring, band)
    return out


def plate_neon(neon):
    s, cr = surface()
    rrect(cr, N * 0.138, N * 0.21 - N * 0.07)
    cr.set_source_rgba(*rgb(neon), 0.85)
    cr.set_line_width(N * 0.0035)
    cr.stroke()
    line = to_pil(s)
    out = glow(line, N * 0.008, 1)
    out.alpha_composite(line)
    return out


def light(variant):
    s, cr = surface()
    g = cairo.RadialGradient(N * 0.28, N * 0.18, 0, N * 0.28, N * 0.18, N * 0.95)
    g.add_color_stop_rgba(0, 1, 1, 1, 0.2 if variant in LIGHT else (0.06 if variant == "smoke" else 0.14))
    g.add_color_stop_rgba(0.5, 1, 1, 1, 0.0)
    g.add_color_stop_rgba(1, 0, 0, 0, 0.16)
    frame_path(cr)
    cr.set_source(g)
    cr.fill()
    return to_pil(s)


def sq(u, v):
    """map the unit disc onto the square, so the wire designs fill the plate with no round outline"""
    r = math.hypot(u, v)
    m = max(abs(u), abs(v))
    return (u, v) if m < 1e-9 else (u * r / m, v * r / m)


def design(kind):
    """the six platinum-wire designs of ICON_COMPONENTS 1.2, as polylines in [-1, 1]^2 (dots as ('dot', u, v))"""
    P = []
    def curve(f, n=240):
        P.append([sq(*f(i / n)) for i in range(n + 1)])
    circ = lambda cx, cy, r: (lambda t: (cx + r * math.cos(TAU * t), cy + r * math.sin(TAU * t)))
    if kind == 0:  # dotted spokes
        for k in range(16):
            a = TAU * k / 16
            P.append([sq(0.34 * math.cos(a), 0.34 * math.sin(a)), sq(0.97 * math.cos(a), 0.97 * math.sin(a))])
            for r in (0.5, 0.66, 0.82):
                P.append(("dot",) + sq(r * math.cos(a + TAU / 32), r * math.sin(a + TAU / 32)))
        curve(circ(0, 0, 0.34))
    elif kind == 1:  # spirograph (hypotrochoid)
        R_, r_, d = 1.0, 0.3125, 0.62
        curve(lambda t: ((R_ - r_) * math.cos(TAU * 5 * t) + d * math.cos((R_ - r_) / r_ * TAU * 5 * t),
                         (R_ - r_) * math.sin(TAU * 5 * t) - d * math.sin((R_ - r_) / r_ * TAU * 5 * t)), 1600)
        P[-1] = [(x / 1.31, y / 1.31) for x, y in P[-1]]
    elif kind == 2:  # eight-point star of two squares, inner octagon
        for rot in (0, TAU / 8):
            P.append([sq(0.96 * math.cos(rot + TAU * k / 4), 0.96 * math.sin(rot + TAU * k / 4)) for k in range(5)])
        P.append([sq(0.52 * math.cos(TAU * k / 8 + TAU / 16), 0.52 * math.sin(TAU * k / 8 + TAU / 16)) for k in range(9)])
    elif kind == 3:  # rosette: twelve circles through the centre
        for k in range(12):
            a = TAU * k / 12
            curve(circ(0.48 * math.cos(a), 0.48 * math.sin(a), 0.48))
    elif kind == 4:  # knotted lattice: interlaced diagonals, broken where they pass under
        g = [-0.9, -0.45, 0.0, 0.45, 0.9]
        for i, c in enumerate(g):
            for sgn in (1, -1):
                pts = [(t, sgn * t + c) for t in np.linspace(-1, 1, 81) if abs(sgn * t + c) <= 1]
                seg = []
                for (u, v) in pts:
                    # leave a gap at every other crossing (the strand passes under)
                    under = any(abs(u - (c2 - c) / (2 * sgn) ) < 0.06 and (i + j + (sgn > 0)) % 2
                                for j, c2 in enumerate(g))
                    if under:
                        if len(seg) > 1: P.append(seg)
                        seg = []
                    else:
                        seg.append((u * 0.97, v * 0.97))
                if len(seg) > 1: P.append(seg)
    else:  # four-petal cross with a square
        for a in (0, TAU / 4, TAU / 2, 3 * TAU / 4):
            ca, sa = math.cos(a), math.sin(a)
            for side in (1, -1):
                pts = []
                for t in np.linspace(0, 1, 60):
                    w = side * 0.32 * math.sin(math.pi * t)
                    u, v = 0.96 * t, w
                    pts.append(sq(u * ca - v * sa, u * sa + v * ca))
                P.append(pts)
        P.append([(0.5 * x, 0.5 * y) for x, y in ((1, 1), (-1, 1), (-1, -1), (1, -1), (1, 1))])
    return P


def plate_wires(pal, variant, seed):
    """platinum wires carved into the square plate, faded where the glyph sits"""
    cx = cy = N / 2
    H = N * (0.5 - 0.124) * 0.9
    s, cr = surface()
    cr.set_line_cap(cairo.LINE_CAP_ROUND)
    cr.set_line_join(cairo.LINE_JOIN_ROUND)
    cr.set_line_width(N * 0.0038)
    cr.set_source_rgb(1, 1, 1)
    for p in design(seed % 6):
        if p[0] == "dot":
            cr.arc(cx + p[1] * H, cy + p[2] * H, N * 0.0075, 0, TAU)
            cr.fill()
            continue
        for i, (u, v) in enumerate(p):
            (cr.move_to if i == 0 else cr.line_to)(cx + u * H, cy + v * H)
        cr.stroke()
    mask = to_pil(s).getchannel("A")
    fade = Image.radial_gradient("L").resize((N, N)).point(lambda v: max(60, min(255, int(60 + (v - 40) * 2.4))))
    mask = ImageChops.multiply(mask, fade)
    out = Image.new("RGBA", (N, N))
    d = max(1, int(N * 0.004))
    if variant in LIGHT:  # a groove: dark line, white lip catching the light below-right
        lip = Image.new("RGBA", (N, N), (255, 255, 255, 0)); lip.putalpha(mask.point(lambda a: a * 3 // 4))
        out.alpha_composite(ImageChops.offset(lip, d, d))
        g = Image.new("RGBA", (N, N), rgb8("#4a4d55")); g.putalpha(mask.point(lambda a: a * 3 // 5))
        out.alpha_composite(g)
    else:  # inlaid platinum: a fine bright line with a faint shine
        line = Image.new("RGBA", (N, N), rgb8(pal["plat"][0])); line.putalpha(mask.point(lambda a: a * 11 // 20))
        out.alpha_composite(glow(line, N * 0.006, 1))
        out.alpha_composite(line)
    return out


def rgb8(h):
    return tuple(int(c * 255) for c in rgb(h)[:3]) + (0,)


def icon(variant, glyph, owl, seed, neon, agent):
    pal = PAL[variant]
    im = halo(neon)
    im.alpha_composite(bevel_frame(pal))
    plate = marble(*pal["plate"], pal["vein"], seed, 0.45 if variant in LIGHT else 0.55,
                   stars=(variant not in LIGHT), guide=(seed // 6) % 4)
    plate.alpha_composite(plate_wires(pal, variant, seed))
    plate.alpha_composite(centre_fade(gears_layer(pal, seed)))
    plate.alpha_composite(plate_neon(neon))
    plate.alpha_composite(filigree_layer(pal, N * 0.124))
    if variant in PLATE_ALPHA:  # see-through plate: nothing of the halo/frame may show under it
        hole = clip_to(Image.new("RGBA", (N, N), (255, 255, 255, 255)), plate_path).getchannel("A")
        im.putalpha(ImageChops.subtract(im.getchannel("A"), hole))
        k = PLATE_ALPHA[variant]
        plate.putalpha(plate.getchannel("A").point(lambda a: int(a * k)))
    im.alpha_composite(clip_to(plate, plate_path))
    im.alpha_composite(inlay_layer(variant, seed))
    if glyph == "owl":
        im.alpha_composite(owl_layer(owl, variant, neon))
    elif glyph:  # None = the bare plate (iconback)
        im.alpha_composite(glyph_layer(glyph, pal, variant, neon))
    im.alpha_composite(light(variant))
    return im


# the pack's icon name -> (Material glyph or "owl", neon, agent). The names are the round pack's, which the
# gnome-map.json below (and the repo's existing one) points the desktop ids at.
TILED = {
    "phone":      ("call", "#3ee68a", False),
    "messaging":  ("chat", "#4fc3ff", False),
    "contacts":   ("person", "#7ad0ff", False),
    "browser":    ("public", "#4fa8ff", False),
    "camera":     ("photo_camera", "#ff5a7a", False),
    "calendar":   ("calendar_month", "#ff7a59", False),
    "clock":      ("schedule", "#ffc94d", False),
    "compass":    ("explore", "#4fd1ff", False),
    "calculator": ("calculate", "#b98bff", False),
    "settings":   ("settings", "#8fb8c8", False),
    "weather":    ("partly_cloudy_day", "#6fd3ff", False),
    "files":      ("folder", "#ffb347", False),
    "terminal":   ("terminal", "#66f0d0", False),
    "gallery":    ("photo_library", "#ff6bd0", False),
    "documents":  ("description", "#b98bff", False),
    "video":      ("play_circle", "#ff5a7a", False),
    "store":      ("shopping_bag", "#ffcc66", False),
    "yesman":     ("owl", "#ffcc66", True),   # our apps' own identity: Minerva's owl in the agent gear ring
    "android":    ("android", "#3ee68a", False),
    "maps":       ("map", "#6fd3ff", False),
    "music":      ("music_note", "#ff4f9a", False),
    "notes":      ("edit_note", "#ffd84d", False),
    "prompt":     ("psychology", "#66f0d0", False),
    "recorder":   ("mic", "#a78bff", False),
    "email":      ("mail", "#5f9dff", False),
    "download":   ("download", "#4fa8ff", False),
}

# desktop id -> drawable (our apps by their own ids, GNOME's by theirs); the round pack's, unchanged
MAP = {
    "de.yesman.Dialer.desktop": "phone", "org.gnome.Calls.desktop": "phone",
    "de.yesman.Messaging.desktop": "messaging", "sm.puri.Chatty.desktop": "messaging",
    "de.yesman.Contacts.desktop": "contacts", "org.gnome.Contacts.desktop": "contacts",
    "de.yesman.NeoBrowser.desktop": "browser", "org.gnome.Epiphany.desktop": "browser",
    "firefox-esr.desktop": "browser",
    "de.yesman.Camera.desktop": "camera", "org.gnome.Snapshot.desktop": "camera",
    "de.yesman.Calendar.desktop": "calendar", "org.gnome.Calendar.desktop": "calendar",
    "de.yesman.Clock.desktop": "clock", "org.gnome.clocks.desktop": "clock",
    "de.yesman.Compass.desktop": "compass",
    "de.yesman.Calculator.desktop": "calculator", "org.gnome.Calculator.desktop": "calculator",
    "de.yesman.NeoSettings.desktop": "settings", "org.gnome.Settings.desktop": "settings",
    "de.yesman.NeoPrompt.desktop": "prompt",
    "de.yesman.NeoWeather.desktop": "weather", "org.gnome.Weather.desktop": "weather",
    "de.yesman.Files.desktop": "files", "org.gnome.Nautilus.desktop": "files",
    "org.gnome.Console.desktop": "terminal", "org.gnome.Ptyxis.desktop": "terminal",
    "org.gnome.Loupe.desktop": "gallery",
    "org.gnome.Papers.desktop": "documents", "org.gnome.Evince.desktop": "documents",
    "org.gnome.Showtime.desktop": "video", "org.gnome.Totem.desktop": "video", "mpv.desktop": "video",
    "org.gnome.Software.desktop": "store",
    "de.yesman.app.desktop": "yesman", "org.minerva.Kiosk.desktop": "yesman",
    "de.yesman.BootAndroid.desktop": "android", "Waydroid.desktop": "android",
    "org.gnome.Maps.desktop": "maps",
    "org.gnome.Music.desktop": "music", "com.github.neithern.g4music.desktop": "music",
    "org.gnome.TextEditor.desktop": "notes",
    "org.gnome.SoundRecorder.desktop": "recorder",
    "org.gnome.Geary.desktop": "email", "org.gnome.Evolution.desktop": "email",
}


def glyph_names():
    """every Material glyph the packs use (fetch-glyphs.sh downloads these)"""
    return sorted({g for g, _, _ in TILED.values()} - {"owl"})


def glyph_path(gdir, name):
    return name if name == "owl" else os.path.join(gdir, name + "_wght200_48px.svg")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("outdir")
    ap.add_argument("--mode", choices=("day", "night"))
    here = os.path.dirname(os.path.abspath(__file__))
    ap.add_argument("--glyphs", default=os.path.join(here, "glyphs"))
    ap.add_argument("--owl", default=os.path.join(here, "owl.png"))
    ap.add_argument("--list-glyphs", action="store_true", help="print the Material glyph names used and exit")
    a = ap.parse_args()
    if a.list_glyphs:
        print("\n".join(glyph_names()))
        return
    if not a.mode:
        ap.error("--mode is required")
    variant = {"day": "milk", "night": "smoke"}[a.mode]
    icons_dir = os.path.join(a.outdir, "icons")
    os.makedirs(icons_dir, exist_ok=True)

    def save(im, name):
        im.resize((PX, PX), Image.LANCZOS).save(os.path.join(icons_dir, name + ".png"), optimize=True)

    for name, (g, neon, agent) in TILED.items():
        save(icon(variant, glyph_path(a.glyphs, g), a.owl, sum(map(ord, name)), neon, agent), name)
    # the plate for apps the pack does not draw: the bare frame, no glyph. pack.json does not list it as an iconback
    # (the framed icons carry their own frame, so an app's own picture is not put on a second plate).
    save(icon(variant, None, a.owl, sum(map(ord, "iconback")), "#8fb8c8", False), "iconback")
    # The launcher (neo-launcher/src/icons.c icons_render) paints a drawn icon at the full icon size, so the framed
    # tile fills the icon box: `scale` only sizes an app's own picture where there is no iconback, and it is moot.
    meta = {"id": "renaissance-" + a.mode, "title": "Renaissance " + a.mode.capitalize(), "scale": 1,
            "shape": None, "iconback": [], "iconmask": [], "iconupon": []}
    with open(os.path.join(a.outdir, "pack.json"), "w") as f:
        json.dump(meta, f, indent=1)
    with open(os.path.join(a.outdir, "gnome-map.json"), "w") as f:
        json.dump(MAP, f, indent=1, sort_keys=True)
    with open(os.path.join(a.outdir, "appfilter.json"), "w") as f:
        json.dump({}, f)
    print(len(TILED) + 1, "icons in", a.outdir)


if __name__ == "__main__":
    main()
