#!/usr/bin/python3 -I
"""make-icon-theme.py PACKS OUT [--prefix P] [--inherits A,B] [--names icon-names.json] PACK...

Turn neo-launcher icon packs (PACKS/<pack>: pack.json, gnome-map.json, icons/*.png, the glyph packs' icons.txt) into
freedesktop icon themes, OUT/<prefix><pack>/ with an index.theme, so the stock setting org.gnome.desktop.interface
icon-theme selects them and every program (Phosh's app grid, GTK apps) uses them through the icon theme with no code
of its own.

  an app pack (gnome-map.json maps desktop ids to drawables): 256x256/apps/<icon name>.png, the icon name being the one
      the app's desktop file asks for: the desktop id without '.desktop', or icon-names.json where it differs
  a glyph pack (icons.txt, no apps): 256x256/status/<name>.png for every name it lists, as they are (the colour is
      part of the glyph, as in Neo Settings)

The directories are Scalable from 8 to 512 px: the PNGs are 256 px squares, GTK scales them. A name the theme lacks
falls to the inherited themes. Output is the packs' files renamed, nothing drawn: rerun after regenerating a pack."""
import argparse
import json
import os
import shutil
import sys

SIZE = 256                 # the packs' squares (tools/iconpack-*: 256 px)
MIN_SIZE, MAX_SIZE = 8, 512


def index_theme(title, comment, inherits, directory, context):
    return (f"[Icon Theme]\nName={title}\nComment={comment}\nInherits={inherits}\nDirectories={directory}\n\n"
            f"[{directory}]\nSize={SIZE}\nMinSize={MIN_SIZE}\nMaxSize={MAX_SIZE}\nType=Scalable\nContext={context}\n")


def build(pack_dir, out_dir, inherits, names):
    with open(os.path.join(pack_dir, "pack.json"), encoding="utf-8") as f:
        meta = json.load(f)
    with open(os.path.join(pack_dir, "gnome-map.json"), encoding="utf-8") as f:
        apps = json.load(f)
    icons = os.path.join(pack_dir, "icons")
    if apps:
        directory, context = f"{SIZE}x{SIZE}/apps", "Applications"
        files = {names.get(desktop, desktop.removesuffix(".desktop")): drawable for desktop, drawable in apps.items()}
    else:
        with open(os.path.join(pack_dir, "icons.txt"), encoding="utf-8") as f:
            files = {n.strip(): n.strip() for n in f if n.strip()}
        directory, context = f"{SIZE}x{SIZE}/status", "Status"
    if os.path.isdir(out_dir):
        shutil.rmtree(out_dir)
    os.makedirs(os.path.join(out_dir, directory))
    missing = []
    for name, drawable in sorted(files.items()):
        src = os.path.join(icons, drawable + ".png")
        if not os.path.isfile(src):
            missing.append(drawable)
            continue
        shutil.copyfile(src, os.path.join(out_dir, directory, name + ".png"))
    if missing:
        sys.exit(f"{pack_dir}: no icons/<drawable>.png for {', '.join(sorted(set(missing)))}")
    comment = "Converted from the neo-launcher icon pack " + meta.get("id", os.path.basename(pack_dir))
    with open(os.path.join(out_dir, "index.theme"), "w", encoding="utf-8") as f:
        f.write(index_theme(meta.get("title", os.path.basename(pack_dir)), comment, inherits, directory, context))
    return len(files)


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("packs")
    ap.add_argument("out")
    ap.add_argument("pack", nargs="+")
    ap.add_argument("--prefix", default="neo-")
    ap.add_argument("--inherits", default="Adwaita,hicolor")
    ap.add_argument("--names", default=os.path.join(here, "icon-names.json"))
    a = ap.parse_args()
    with open(a.names, encoding="utf-8") as f:
        names = {k: v for k, v in json.load(f).items() if not k.startswith("_")}
    destdir = os.environ.get("DESTDIR", "")
    out = destdir + a.out if destdir and os.path.isabs(a.out) else a.out
    for pack in a.pack:
        n = build(os.path.join(a.packs, pack), os.path.join(out, a.prefix + pack), a.inherits, names)
        print(f"{a.prefix}{pack}: {n} icons")


if __name__ == "__main__":
    main()
