/* ns-look: the Renaissance look of Neo Settings, in its four variants (Marmo, Marino by day; Bosco, Prugna by night).
 *
 * The look in force is the one libneo and the shell work out: none while the Theme setting name (de.yesman.GnomePhone.
 * Theme) is not 'renaissance'; else the night-look when org.gnome.desktop.interface color-scheme is prefer-dark, the
 * day-look when it is not. It follows all of them live: the window carries ns-ren (any look), ns-look-day or
 * ns-look-night, and ns-look-<id>; the tokens style.css names (@ns_gold, @ns_card_a ..., generated from
 * tools/theme/palette.json into looks/ns-look-<id>.css by tools/theme/gen_looks.py) are swapped by reloading the one
 * stylesheet with the look's sheet in front of it; and the rows' icons switch between the icon theme's and the look's
 * bare-glyph pack. */
#pragma once

#include <gtk/gtk.h>

/* Loads style.css (with the tokens) for the display and puts the look's classes on `win`; both follow the settings.
 * Call once, from the window's creation. */
void        ns_look_init (GtkWidget *win);
/* "marble", "sea", "forest" or "plum", NULL in the stock look */
const char *ns_look (void);
gboolean    ns_look_is_day (void);
/* Makes `image` show the icon `icon_name`: from the icon theme, or, while a Renaissance look is in force and its glyph
 * pack (neo-launcher/iconpacks/renaissance-glyph-day or -night, found through the data dirs like the launcher does)
 * has <icon_name>.png, that bare glyph. The image keeps following the look until it is destroyed. */
void        ns_look_set_icon (GtkImage *image, const char *icon_name);
