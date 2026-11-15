#!/usr/bin/python3 -I
# make-renaissance-pack.py OUTDIR --style tile|glyph --mode day|night [--glyphs DIR] [--owl PNG]
#
# The four Renaissance icon packs of the theme (neo-launcher/iconpacks/renaissance-*), generated:
#   tile day    (marble/sea):  renaissance-day        the look's quick-settings inactive tile: a flat rounded square in
#   tile night  (forest/plum): renaissance-night      the tile fill, its Material glyph centred in the tile's ink, no
#                                                     frame, no neon. Full-bleed, for the launcher's home grid and dock;
#                                                     `agent` icons (Minerva) carry the owl. Day is the day looks' cream
#                                                     tile with dark ink, night the dark tile with light ink.
#   glyph day    (marble):  renaissance-glyph-day    only the inner part: the fine-line glyph with its neon aura, on
#   glyph night  (granite): renaissance-glyph-night  transparent, for Neo Settings' rows and search results.
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

# The quick-settings tile these home icons copy (extension/qs.js): TILE_W 76 dp, TILE_RADIUS 24 dp, a flat fill and
# TILE_COLORS.inactive's colours — the look's effect1 colour over its shade panel, the glyph in the look's on-surface.
# One pack serves two looks, so the day fill is marble's and sea's cream (both ~#fafaf7 at .8 over the panel) with their
# ink, the night fill forest's and plum's near-black with their light ink.
TILE_R = 24 / 76  # the tile's corner radius / its width
TILE = {
    "day": dict(fill="#faf7ef", ink="#2b2c2d"),  # marble #fafaf7 / #2b2c31, sea #fbf7ee / #2c2b29
    "night": dict(fill="#0e1215", ink="#ece7d8"),  # forest #0c1611 / #e6efe4, plum #100f1a / #f0e6d0
}
GLYPH_K = 0.50  # the glyph's side / the tile's: a quick-settings glyph is 96/228
GLYPH_SW = 26  # its stroke: weight 200 outlined, fattened so it reads solid like a quick-settings glyph


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


def tile(variant, glyph, owl):
    """a home icon: the look's quick-settings inactive tile, its glyph (or the owl) flat and centred; glyph None = the
    blank plate (no glyph)"""
    t = TILE[variant]
    s, cr = surface()
    rrect(cr, 0, N * TILE_R)
    cr.set_source_rgb(*rgb(t["fill"]))
    cr.fill()
    out = to_pil(s)
    if glyph == "owl":
        owl_im = Image.open(owl).convert("RGBA")
        w = int(N * 0.60)
        h = int(owl_im.height * w / owl_im.width)
        out.alpha_composite(owl_im.resize((w, h), Image.LANCZOS), ((N - w) // 2, (N - h) // 2))
    elif glyph:
        out.alpha_composite(render_svg(glyph, t["ink"], N * GLYPH_K, sw=GLYPH_SW))
    return out


def bare_icon(variant, glyph, neon):
    """a glyph-pack icon: the fine-line glyph (or the owl) and its neon aura on transparent"""
    if glyph == "owl":
        return owl_layer(OWL, neon, BARE_K)
    return glyph_layer(glyph, variant, neon, BARE_K)


# ---- what is drawn

# tile packs: the pack's icon name -> (Material glyph or "owl"); these are the names the launcher's gnome-map.json
# points the desktop ids at (the round pack's names)
TILED = {
    "phone":      ("call",),
    "messaging":  ("chat",),
    "contacts":   ("person",),
    "browser":    ("public",),
    "camera":     ("photo_camera",),
    "calendar":   ("calendar_month",),
    "clock":      ("schedule",),
    "compass":    ("explore",),
    "calculator": ("calculate",),
    "settings":   ("settings",),
    "weather":    ("partly_cloudy_day",),
    "files":      ("folder",),
    "terminal":   ("terminal",),
    "gallery":    ("photo_library",),
    "documents":  ("description",),
    "video":      ("play_circle",),
    "store":      ("shopping_bag",),
    "yesman":     ("owl",),     # our apps' own identity: Minerva's owl
    "android":    ("android",),
    "maps":       ("map",),
    "music":      ("music_note",),
    "notes":      ("edit_note",),
    "recorder":   ("mic",),
    "email":      ("mail",),
    "download":   ("download",),
}

# desktop id -> drawable (our apps by their own ids, GNOME's by theirs); the round pack's
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
    "de.yesman.NeoWeather.desktop": "weather", "org.gnome.Weather.desktop": "weather",
    "de.yesman.Files.desktop": "files", "org.gnome.Nautilus.desktop": "files",
    "org.gnome.Console.desktop": "terminal", "org.gnome.Ptyxis.desktop": "terminal",
    "org.gnome.Loupe.desktop": "gallery",
    "org.gnome.Papers.desktop": "documents", "org.gnome.Evince.desktop": "documents",
    "org.gnome.Showtime.desktop": "video", "org.gnome.Totem.desktop": "video", "mpv.desktop": "video",
    "org.gnome.Software.desktop": "store",
    "de.yesman.app.desktop": "yesman",
    "de.yesman.BootAndroid.desktop": "android", "Waydroid.desktop": "android",
    "org.gnome.Maps.desktop": "maps",
    "org.gnome.Music.desktop": "music", "com.github.neithern.g4music.desktop": "music",
    "org.gnome.TextEditor.desktop": "notes",
    "org.gnome.SoundRecorder.desktop": "recorder",
    "org.gnome.Geary.desktop": "email", "org.gnome.Evolution.desktop": "email",
}

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
    names = {g for (g,) in TILED.values()} | {g for g, _ in GLYPHS.values()}
    return sorted(names - {"owl"})


def glyph_path(gdir, name):
    return name if name == "owl" else os.path.join(gdir, name + "_wght200_48px.svg")


def main():
    global OWL
    ap = argparse.ArgumentParser()
    ap.add_argument("outdir")
    ap.add_argument("--style", choices=("tile", "glyph"))
    ap.add_argument("--mode", choices=("day", "night"))
    here = os.path.dirname(os.path.abspath(__file__))
    ap.add_argument("--glyphs", default=os.path.join(here, "glyphs"))
    ap.add_argument("--owl", default=os.path.join(here, "owl.png"))
    ap.add_argument("--list-glyphs", action="store_true", help="print the Material glyph names used and exit")
    a = ap.parse_args()
    if a.list_glyphs:
        print("\n".join(glyph_names()))
        return
    if not (a.style and a.mode):
        ap.error("--style and --mode are required")
    OWL = a.owl
    variant = {("tile", "day"): "day", ("tile", "night"): "night",
               ("glyph", "day"): "marble", ("glyph", "night"): "granite"}[(a.style, a.mode)]
    pack_id = "renaissance-" + ("" if a.style == "tile" else "glyph-") + a.mode
    title = "Renaissance " + ("Glyph " if a.style == "glyph" else "") + a.mode.capitalize()
    icons_dir = os.path.join(a.outdir, "icons")
    os.makedirs(icons_dir, exist_ok=True)

    def save(im, name):
        im.resize((PX, PX), Image.LANCZOS).save(os.path.join(icons_dir, name + ".png"), optimize=True)

    if a.style == "tile":
        for (name, (g,)) in TILED.items():
            save(tile(variant, glyph_path(a.glyphs, g), OWL), name)
        # the plate for apps the pack does not draw: the tile alone, no glyph
        save(tile(variant, None, OWL), "iconback")
        # The launcher (neo-launcher/src/icons.c icons_render) paints a drawn icon at the full icon size, so the tile
        # already fills the icon box: scale only sizes the app's own picture on the iconback, at the tile's glyph size.
        meta = {"id": pack_id, "title": title, "scale": 0.5, "shape": None,
                "iconback": ["iconback"], "iconmask": [], "iconupon": []}
        with open(os.path.join(a.outdir, "gnome-map.json"), "w") as f:
            json.dump(MAP, f, indent=1, sort_keys=True)
        with open(os.path.join(a.outdir, "appfilter.json"), "w") as f:
            json.dump({}, f)
        count = len(TILED)
    else:
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
