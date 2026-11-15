#!/bin/bash
# fetch-glyphs.sh [GLYPHDIR]: download the Material Symbols (Outlined, weight 200, 48 px) the generator uses from
# github.com/google/material-design-icons (Apache-2.0) into glyphs/, with the licence.
set -e
here=$(cd "$(dirname "$0")" && pwd)
out=${1:-$here/glyphs}
base=https://raw.githubusercontent.com/google/material-design-icons/master
mkdir -p "$out"
for n in $(python3 -I "$here/make-renaissance-pack.py" x --list-glyphs; python3 -I "$here/make-tiles.py" x --list-glyphs); do
  curl -fsS "$base/symbols/web/$n/materialsymbolsoutlined/${n}_wght200_48px.svg" -o "$out/${n}_wght200_48px.svg"
done
curl -fsS "$base/LICENSE" -o "$out/LICENSE"
