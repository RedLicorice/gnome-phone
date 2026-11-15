#!/usr/bin/python3 -I
"""gen_app_overrides.py OUTDIR APPDIR...: the Renaissance look for the apps that carry their own palette
(Messaging, Dialer, Contacts, Calendar ...), as two sheets in OUTDIR: neo-renaissance-apps-night.css (for Bosco and
Prugna) and neo-renaissance-apps-day.css (for Marmo and Marino). For every CSS rule of the given apps that names a
colour, emit the same selector with the colour moved into the theme.
  night: teal becomes lapis (dark) or gold (light), dark neutrals take the night's violet cast, white becomes parchment.
         The polarity of the app's own palette is kept, so a pair that read in the app reads here too.
  day:   the lightness is turned over for neutrals (dark surfaces light, light text dark ink), dark teal becomes pale
         gold, pastel teal deep gold; other hues stay in both. A day look is light: an opaque surface is never allowed
         to stay dark and a text colour is never allowed to stay light, or a pair the app drew light-on-dark would come
         out dark-on-dark (and the other way round) after the two ends were moved one at a time.
Colours are read in every syntax the apps use: #rrggbb, rgb(), rgba() and the CSS named colours. Alpha is kept.

Loaded by libneo above the apps' own sheets (priority USER), the one of the look's scheme. libneo's neo_theme_map_rgba
(neo-style.c) is the same mapping for the colours drawn in C: keep them in step.

Generated: change the mapping here, not in the output."""
import colorsys
import glob
import os
import re
import sys

HEX = re.compile(r"#([0-9a-fA-F]{6})\b")
COLOR = re.compile(r"#[0-9a-fA-F]{6}\b|rgba?\([^()]*\)")
NAMED = {
    "white": (255, 255, 255), "black": (0, 0, 0), "silver": (192, 192, 192), "gray": (128, 128, 128),
    "grey": (128, 128, 128), "red": (255, 0, 0), "maroon": (128, 0, 0), "orange": (255, 165, 0),
    "yellow": (255, 255, 0), "olive": (128, 128, 0), "lime": (0, 255, 0), "green": (0, 128, 0),
    "teal": (0, 128, 128), "aqua": (0, 255, 255), "cyan": (0, 255, 255), "blue": (0, 0, 255),
    "navy": (0, 0, 128), "purple": (128, 0, 128), "fuchsia": (255, 0, 255), "magenta": (255, 0, 255),
    "pink": (255, 192, 203), "brown": (165, 42, 42), "gold": (255, 215, 0),
}
NAMED_RE = re.compile(r"(?<![\w-])(%s)(?![\w-])" % "|".join(NAMED), re.I)
SKIP = ("-light.css", "-black.css")
# a day look is light: these bounds keep a pair readable after both of its ends were moved
SURFACE_PROPS = ("background", "background-color", "background-image")


def to_hls(rgb):
    return colorsys.rgb_to_hls(*(c / 255 for c in rgb))


def from_hls(h, l, s):
    r, g, b = colorsys.hls_to_rgb(h % 1, max(0, min(1, l)), max(0, min(1, s)))
    return "#%02x%02x%02x" % (round(r * 255), round(g * 255), round(b * 255))


def remap_night(h):
    hue, l, s = to_hls(parse_hex(h))
    deg = hue * 360
    if s < 0.14:                                  # neutrals
        if l > 0.93:
            return "#f0e6d0"                      # white text -> parchment
        if l > 0.75:
            return from_hls(41 / 360, l * 0.97, 0.22)    # light greys -> warm parchment greys
        if l < 0.45:
            return from_hls(250 / 360, l * 0.55, 0.28)   # dark greys -> the night
        return from_hls(41 / 360, l * 0.9, 0.10)
    if 140 <= deg <= 230:                         # teal and cyan family
        if l < 0.42:
            return from_hls(224 / 360, l * 0.85, 0.52)   # dark teal -> lapis
        return from_hls(41 / 360, min(l, 0.78), 0.55)    # light teal -> gold
    return h                                     # other hues (red, orange, calendar colours) stay


def remap_day(h):
    hue, l, s = to_hls(parse_hex(h))
    deg = hue * 360
    if s < 0.14:                                  # neutrals: the lightness turned over
        if l > 0.93:
            return "#2b2c31"                      # white text -> ink
        if l > 0.75:
            return from_hls(225 / 360, 0.2 + (1 - l) * 0.9, 0.08)   # light greys -> dark greys
        if l < 0.45:
            return from_hls(40 / 360, 0.97 - l * 0.45, 0.10)        # dark surfaces -> pale warm marble
        return from_hls(225 / 360, l * 0.85, 0.06)                  # mid greys stay mid, a little darker
    if 140 <= deg <= 230:                         # teal and cyan family
        if l < 0.42:
            return from_hls(41 / 360, 0.97 - l * 0.45, 0.45)        # dark teal (bars, containers) -> pale gold
        return from_hls(38 / 360, 0.62 - (min(l, 0.78) - 0.42) * 0.55, 0.52)   # pastel teal -> deep gold
    return "#" + h


def parse_hex(h):
    h = h.lstrip("#")
    return tuple(int(h[i:i + 2], 16) for i in (0, 2, 4))


def lum(rgb):
    f = lambda v: v / 12.92 if v <= 0.03928 else ((v + 0.055) / 1.055) ** 2.4
    r, g, b = (f(c / 255) for c in rgb)
    return 0.2126 * r + 0.7152 * g + 0.0722 * b


def contrast(a, b):
    la, lb = lum(a), lum(b)
    return (max(la, lb) + 0.05) / (min(la, lb) + 0.05)


def over(fg, alpha, bg):
    return tuple(round(f * alpha + b * (1 - alpha)) for f, b in zip(fg, bg))


WINDOW = {"day": (246, 246, 244), "night": (8, 7, 13)}   # the window the apps draw on, per scheme
TARGET = 4.5                                     # WCAG AA for body text


DAY_PLATE = (235, 229, 221)                     # the lightest plate a day fill can be (neo_theme_map_rgba lifts to 0.9)


def _walk(rgb, alpha, bgs, dark):
    """the nearest lightness toward `dark` that reads on every surface in bgs, or None"""
    hue, l, s = to_hls(rgb)
    ok = lambda c: all(contrast(over(c, alpha, b), b) >= TARGET for b in bgs)
    if ok(rgb):
        return rgb
    step = -0.02 if dark else 0.02
    while 0 <= l + step <= 1:
        l += step
        c = parse_hex(from_hls(hue, l, s))
        if ok(c):
            return c
    return None


def solve(rgb, alpha, bgs, prefer_dark):
    """the same hue and saturation at a lightness that reads on every surface in `bgs` (the rule's own or its
    ancestor's, plus — by day — the lightest plate a fill can be). Alpha gives way first: a dim label is a label."""
    hue, l, s = to_hls(rgb)
    while alpha < 0.999 and any(contrast(over(rgb, alpha, b), b) < TARGET for b in bgs):
        alpha = min(1.0, alpha + 0.05)
    for pool in (bgs, bgs[:1]):
        for dark in (prefer_dark, not prefer_dark):
            c = _walk(rgb, alpha, pool, dark)
            if c:
                return c, round(alpha, 3)
    return rgb, round(alpha, 3)


def map_rgb(rgb, alpha, role, scheme, bg=None):
    """one colour, by scheme and by what it is for: text, a surface, or anything else (shadows, borders).
    Text is resolved against the surface it sits on (`bg`, the rule's own or its ancestor's, else the window)."""
    h = "%02x%02x%02x" % rgb
    new = remap_day(h) if scheme == "day" else remap_night(h)
    if role == "text":
        ref = bg or WINDOW[scheme]
        pool = [ref, DAY_PLATE] if scheme == "day" else [ref]
        return solve(parse_hex(new.lstrip("#")), alpha, pool, scheme == "day")
    return parse_hex(new.lstrip("#")), alpha


def parse_token(tok):
    """(rgb, alpha) of one colour token, or None (a named colour is looked up, rgb() is parsed)"""
    m = HEX.match(tok)
    if m:
        return parse_hex(m.group(1)), 1.0
    m = re.fullmatch(r"rgba?\(\s*([\d.]+)\s*,\s*([\d.]+)\s*,\s*([\d.]+)\s*(?:,\s*([\d.]+)\s*)?\)", tok)
    if m:
        a = float(m.group(4)) if m.group(4) else 1.0
        return (round(float(m.group(1))), round(float(m.group(2))), round(float(m.group(3)))), a
    rgb = NAMED.get(tok.lower())
    return (rgb, 1.0) if rgb else None


def fmt(rgb, alpha):
    if alpha >= 0.999:
        return "#%02x%02x%02x" % rgb
    return "rgba(%d, %d, %d, %s)" % (rgb[0], rgb[1], rgb[2], ("%.3f" % alpha).rstrip("0").rstrip("."))


def convert(value, role, scheme, bg=None):
    def sub_hex(m):
        rgb, a = parse_token(m.group(0))
        return fmt(*map_rgb(rgb, a, role, scheme, bg))

    def sub_named(m):
        rgb, a = parse_token(m.group(0))
        return fmt(*map_rgb(rgb, a, role, scheme, bg))

    # a shadow, a border or a gradient's stop is left as it was, unless it is a hex colour (as it always was):
    # turning a scrim light would paint a glow, not a day look
    if role == "other" or (role == "surface" and value.lstrip().startswith("linear-gradient")):
        return HEX.sub(lambda m: "#" + remap_day(m.group(1)) if scheme == "day" else "#" + remap_night(m.group(1)),
                       value)
    return NAMED_RE.sub(sub_named, COLOR.sub(sub_hex, value))


RULE = re.compile(r"([^{}@]+)\{([^{}]*)\}", re.S)


def role_of(prop):
    if prop in ("color", "caret-color"):
        return "text"
    if prop in SURFACE_PROPS:
        return "surface"
    return "other"


def decls_of(body):
    out = []
    for x in body.split(";"):
        if ":" in x:
            prop, value = (s.strip() for s in x.split(":", 1))
            out.append((prop, value))
    return out


def tokens(sel):
    return [t for t in re.split(r"\s+", sel.replace(">", " ")) if t]


def is_ancestor(anc, sel):
    """`anc` names an element this rule draws inside (a compound-prefix of one of its comma parts)"""
    ta = tokens(anc)
    return any(len(ta) < len(tb) and tb[:len(ta)] == ta for tb in (tokens(s) for s in sel.split(",")))


def first_colour(value):
    m = COLOR.search(value) or NAMED_RE.search(value)
    return parse_token(m.group(0)) if m else None


def sheet(dirs, scheme):
    lines = ["/* GENERATED by tools/theme/gen_app_overrides.py: the Renaissance look for the apps' own palettes, "
             "%s looks. */" % scheme]
    for d in dirs:
        for path in sorted(glob.glob(os.path.join(d, "src", "*.css"))):
            if path.endswith(SKIP):
                continue
            text = re.sub(r"/\*.*?\*/", "", open(path).read(), flags=re.S)
            lines.append(f"/* {os.path.relpath(path, os.path.dirname(d))} */")
            rules = [(" ".join(sel.split()), decls_of(body)) for sel, body in RULE.findall(text)]
            # the surface each rule draws on: its own background, else the nearest ancestor's in this file
            bgs = []
            for sel, decls in rules:
                c = None
                for prop, value in decls:
                    if prop in SURFACE_PROPS:
                        c = first_colour(convert(value, "surface", scheme))
                        if c:
                            break
                bgs.append(c)
            for i, (sel, decls) in enumerate(rules):
                keep = []
                for prop, value in decls:
                    bg = None
                    if role_of(prop) == "text":
                        bg = bgs[i]
                        for order in (range(i - 1, -1, -1), range(i + 1, len(rules))):
                            for j in order:
                                if bgs[j] and is_ancestor(rules[j][0], sel):
                                    bg = bgs[j]
                                    break
                            if bg:
                                break
                        bg = bg or (WINDOW[scheme], 1.0)
                        if bg[1] < 0.999:                       # an overlay: what shows through is the window
                            bg = (over(bg[0], bg[1], WINDOW[scheme]), 1.0)
                    new = convert(value, role_of(prop), scheme, bg[0] if bg else None)
                    if new != value:
                        keep.append("%s: %s;" % (prop, new))
                if keep:
                    lines.append(sel + " { " + " ".join(keep) + " }")
    return lines


def main():
    out, dirs = sys.argv[1], sys.argv[2:]
    os.makedirs(out, exist_ok=True)
    for scheme in ("night", "day"):
        path = os.path.join(out, f"neo-renaissance-apps-{scheme}.css")
        lines = sheet(dirs, scheme)
        open(path, "w").write("\n".join(lines) + "\n")
        print(path, len(lines), "lines")


if __name__ == "__main__":
    main()
