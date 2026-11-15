#!/bin/bash
# The look machinery (theme.js, palette.js) on a memory GSettings backend, under plain gjs:
#   extension/tests/theme-test.sh
set -e
# palette.js imports Cogl (mutter's typelib): without it there is nothing to run (meson: skipped)
mutter=$(ls -d /usr/lib/*/mutter-* 2>/dev/null | head -1)
{ [ -n "$mutter" ] && command -v glib-compile-schemas >/dev/null; } || { echo "skipped: no mutter typelibs or glib-compile-schemas"; exit 77; }
here=$(cd "$(dirname "$0")" && pwd)
work=$(mktemp -d "${XDG_CACHE_HOME:-$HOME/.cache}/gnome-phone-theme-test.XXXXXX")
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/schemas" "$work/data"
# the two system schemas the shell reads, as far as theme.js uses them
cat > "$work/schemas/org.gnome.desktop.gschema.xml" <<'XML'
<schemalist>
<schema id="org.gnome.desktop.interface" path="/org/gnome/desktop/interface/"><key name="color-scheme" type="s"><default>'default'</default></key></schema>
<schema id="org.gnome.desktop.background" path="/org/gnome/desktop/background/"><key name="picture-uri" type="s"><default>''</default></key><key name="picture-uri-dark" type="s"><default>''</default></key></schema>
</schemalist>
XML
cp "$here/../schemas/de.yesman.GnomePhone.Theme.gschema.xml" "$work/schemas/"
cat > "$work/schemas/de.yesman.neo.gschema.xml" <<'XML'
<schemalist><schema id="de.yesman.neo" path="/de/yesman/neo/"><key name="icon-pack" type="s"><default>''</default></key></schema></schemalist>
XML
glib-compile-schemas "$work/schemas"
for look in marble sea forest plum; do
    mkdir -p "$work/data/neo-launcher/parallax/renaissance-$look"; touch "$work/data/neo-launcher/parallax/renaissance-$look/wallpaper.jpg"
done
for pack in renaissance-day renaissance-night; do
    mkdir -p "$work/data/neo-launcher/iconpacks/$pack"; echo '{}' > "$work/data/neo-launcher/iconpacks/$pack/pack.json"
done
export GSETTINGS_BACKEND=memory GSETTINGS_SCHEMA_DIR="$work/schemas" XDG_DATA_HOME="$work/data" XDG_DATA_DIRS="$work/data"
export GI_TYPELIB_PATH="$mutter:$GI_TYPELIB_PATH"
gjs -m "$here/theme.test.js"
