#!/usr/bin/python3 -I
# make-renaissance-pack.py OUTDIR --mode day|night [--glyphs DIR] [--owl PNG]
#
# The two Renaissance glyph packs of the theme (neo-launcher/iconpacks/renaissance-glyph-{day,night}), generated: only
# the inner part of the mock-up's icon — the fine-line glyph with its neon aura on transparent, in a marble (day) or
# granite (night) ink — for Neo Settings' rows and search results, not the launcher. The home-grid packs (the framed
# tiles, renaissance-{day,night}) are make-tiles.py.
# Glyphs: Material Symbols Outlined, weight 200 (Google, Apache-2.0; glyphs/LICENSE, fetched by fetch-glyphs.sh).
# Deterministic. Needs python3-gi with Rsvg 2.0, python3-cairo and python3-pil.
import argparse
import json
import math
import os

import cairo
import gi
gi.require_version("Rsvg", "2.0")
from gi.repository import Rsvg
from PIL import Image, ImageFilter

PX, SS = 256, 4
N = PX * SS
BARE_K = 1.36  # the glyph packs' glyph and owl scale: no frame around them, so they fill the icon
OWL = None  # the owl PNG, set by main()
TAU = math.tau



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


def glow(line, radius, times=2):
    g = line.filter(ImageFilter.GaussianBlur(radius))
    out = Image.new("RGBA", (N, N))
    for _ in range(times):
        out.alpha_composite(g)
    return out


# ---- components

# the glyph packs' ink (a marble or granite ground): the deep grey of the fine-line glyph in each look
GLYPH_INK = {"granite": "#f3dc9f", "marble": "#2a2b2e", "sea": "#2a2b2e", "forest": "#efe0b0"}
LIGHT = {"marble", "sea"}


def render_svg(path, colour, size, dx=0, dy=0, sw=14):
    with open(path) as f:
        svg = f.read().replace("<svg ", '<svg fill="%s" stroke="%s" stroke-width="%d" stroke-linejoin="round" '
                               % (colour, colour, sw), 1)
    h = Rsvg.Handle.new_from_data(svg.encode())
    s, cr = surface()
    r = Rsvg.Rectangle()
    r.x, r.y, r.width, r.height = (N - size) / 2 + dx, (N - size) / 2 + dy, size, size
    h.render_document(cr, r)
    return to_pil(s)


def glyph_layer(path, variant, neon, k=1.0):
    """a glyph-pack icon's glyph: the fine line in the look's ink with its neon aura and a chiselled lip"""
    size = N * 0.58 * k

    def render(colour, dx=0, dy=0):
        return render_svg(path, colour, size, dx, dy)

    def fade(im, k):
        im.putalpha(im.getchannel("A").point(lambda a: int(a * k)))
        return im

    out = Image.new("RGBA", (N, N))
    if variant in LIGHT:  # deep ink tinted toward the neon, a soft neon aura, a chiselled white lip
        out.alpha_composite(fade(glow(render(neon), N * 0.016, 2), 0.85))
        out.alpha_composite(render("#ffffff", N * 0.003, N * 0.004))
        out.alpha_composite(render(mix(GLYPH_INK[variant], neon, 0.38)))
    else:  # pale gold tinted toward the neon, full neon glow, a dark cast shadow
        out.alpha_composite(glow(render(neon), N * 0.012, 2))
        out.alpha_composite(render("#000000", N * 0.003, N * 0.005))
        out.alpha_composite(render(mix(GLYPH_INK[variant], neon, 0.30)))
    return out


def owl_layer(owl_path, neon, k=1.0):
    owl = Image.open(owl_path).convert("RGBA")
    w = int(N * 0.66 * k)
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



def bare_icon(variant, glyph, neon):
    """a glyph-pack icon: the fine-line glyph (or the owl) and its neon aura on transparent"""
    if glyph == "owl":
        return owl_layer(OWL, neon, BARE_K)
    return glyph_layer(glyph, variant, neon, BARE_K)


# ---- what is drawn



# glyph packs: the icon name -> (Material glyph or "owl", neon). The names are the icon-name strings Neo Settings uses
# (apps/settings/src: rows, pages, level rows, the Bluetooth device kinds), "-symbolic" suffix kept, and the mock-up's
# short names. The colour is the area's: connections cyan/blue, sound violet, notifications orange, display yellow,
# battery green, accounts sky, lock gold, apps aqua, Minerva's owl gold.
BLUE, CYAN, VIOLET, ORANGE, YELLOW, GREEN, SKY, GOLD, AQUA, GREY, PINK, RED = (
    "#5b7cff", "#4fd1ff", "#a78bff", "#ff8a4d", "#ffd84d", "#4ee68f", "#7ad0ff", "#ffcf5c", "#66f0d0", "#8fb8c8",
    "#ff6bd0", "#ff5a7a")
GLYPHS = {
    # the mock-up's names
    "wifi": ("wifi", CYAN), "bt": ("bluetooth", BLUE), "display": ("brightness_6", YELLOW),
    "sound": ("volume_up", VIOLET), "notif": ("notifications", ORANGE), "battery": ("battery_full", GREEN),
    "account": ("person", SKY), "lock": ("lock", GOLD), "minerva": ("owl", "#ffcc66"), "apps": ("apps", AQUA),
    # Neo Settings
    "action-unavailable-symbolic": ("block", GREY),
    "airplane-mode-symbolic": ("flight", ORANGE),
    "alarm-symbolic": ("alarm", VIOLET),
    "application-x-executable": ("widgets", AQUA),
    "audio-headphones-symbolic": ("headphones", BLUE),
    "audio-input-microphone-symbolic": ("mic", VIOLET),
    "audio-speakers-symbolic": ("speaker", BLUE),
    "audio-volume-high-symbolic": ("volume_up", VIOLET),
    "auth-fingerprint-symbolic": ("fingerprint", GOLD),
    "avatar-default-symbolic": ("person", SKY),
    "battery-full-symbolic": ("battery_full", GREEN),
    "battery-level-80-symbolic": ("battery_5_bar", GREEN),
    "bluetooth-symbolic": ("bluetooth", BLUE),
    "call-start-symbolic": ("call", VIOLET),
    "camera-photo-symbolic": ("photo_camera", RED),
    "channel-secure-symbolic": ("verified_user", GOLD),
    "computer-symbolic": ("computer", GREY),
    "dialog-password-symbolic": ("key", GOLD),
    "display-brightness-symbolic": ("brightness_6", YELLOW),
    "document-open-symbolic": ("folder_open", "#ffb347"),
    "emblem-shared-symbolic": ("share", ORANGE),
    "emblem-system-symbolic": ("settings", GREY),
    "face-smile-symbolic": ("owl", "#ffcc66"),
    "find-location-symbolic": ("location_on", "#b98bff"),
    "go-down-symbolic": ("keyboard_arrow_down", GREY),
    "go-up-symbolic": ("keyboard_arrow_up", GREY),
    "help-about-symbolic": ("info", GREY),
    "input-gaming-symbolic": ("sports_esports", BLUE),
    "input-keyboard-symbolic": ("keyboard", GREY),
    "input-mouse-symbolic": ("mouse", GREY),
    "list-add-symbolic": ("add", "#4fa8ff"),
    "list-remove-symbolic": ("remove", RED),
    "network-cellular-symbolic": ("signal_cellular_alt", "#3ee68a"),
    "network-server-symbolic": ("dns", "#b98bff"),
    "network-vpn-symbolic": ("vpn_key", "#b98bff"),
    "network-wired-symbolic": ("lan", "#b98bff"),
    "network-wireless-hotspot-symbolic": ("wifi_tethering", "#3ee68a"),
    "network-wireless-signal-excellent-symbolic": ("wifi", CYAN),
    "network-wireless-signal-good-symbolic": ("wifi", CYAN),
    "network-wireless-signal-ok-symbolic": ("wifi_2_bar", CYAN),
    "network-wireless-signal-weak-symbolic": ("wifi_1_bar", CYAN),
    "network-wireless-symbolic": ("wifi", CYAN),
    "object-select-symbolic": ("check", GREEN),
    "phone-symbolic": ("smartphone", GREY),
    "preferences-desktop-accessibility-symbolic": ("accessibility_new", GREEN),
    "preferences-desktop-locale-symbolic": ("language", "#6fd3ff"),
    "preferences-desktop-wallpaper-symbolic": ("wallpaper", PINK),
    "preferences-system-notifications-symbolic": ("notifications", ORANGE),
    "preferences-system-symbolic": ("settings", GREY),
    "preferences-system-time-symbolic": ("schedule", "#ffc94d"),
    "printer-symbolic": ("print", GREY),
    "security-high-symbolic": ("security", GOLD),
    "software-update-available-symbolic": ("system_update", "#4fa8ff"),
    "system-lock-screen-symbolic": ("lock", GOLD),
    "system-search-symbolic": ("search", "#4fa8ff"),
    "system-users-symbolic": ("group", ORANGE),
    "user-trash-symbolic": ("delete", RED),
    "view-app-grid-symbolic": ("apps", AQUA),
    "view-grid-symbolic": ("grid_view", AQUA),
}


def glyph_names():
    """every Material glyph the packs use (fetch-glyphs.sh downloads these)"""
    names = {g for g, _ in GLYPHS.values()}
    return sorted(names - {"owl"})


def glyph_path(gdir, name):
    return name if name == "owl" else os.path.join(gdir, name + "_wght200_48px.svg")


def main():
    global OWL
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
    OWL = a.owl
    variant = {"day": "marble", "night": "granite"}[a.mode]
    pack_id = "renaissance-glyph-" + a.mode
    title = "Renaissance Glyph " + a.mode.capitalize()
    icons_dir = os.path.join(a.outdir, "icons")
    os.makedirs(icons_dir, exist_ok=True)

    def save(im, name):
        im.resize((PX, PX), Image.LANCZOS).save(os.path.join(icons_dir, name + ".png"), optimize=True)

    for name, (g, neon) in GLYPHS.items():
        save(bare_icon(variant, glyph_path(a.glyphs, g), neon), name)
    # a pack with no drawn apps: Neo Settings and the search read icons/<name>.png and icons.txt, not the launcher
    meta = {"id": pack_id, "title": title, "scale": 1, "shape": None,
            "iconback": [], "iconmask": [], "iconupon": []}
    with open(os.path.join(a.outdir, "gnome-map.json"), "w") as f:
        f.write("{}")
    with open(os.path.join(a.outdir, "appfilter.json"), "w") as f:
        f.write("{}")
    with open(os.path.join(a.outdir, "icons.txt"), "w") as f:
        f.write("".join(n + "\n" for n in sorted(GLYPHS)))
    count = len(GLYPHS)
    with open(os.path.join(a.outdir, "pack.json"), "w") as f:
        json.dump(meta, f, indent=1)
    print(count, "icons in", a.outdir)


if __name__ == "__main__":
    main()
