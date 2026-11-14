/* neo-style: the colours, the font and the stylesheet every libneo app shares.
 *
 * Call neo_init () once, from the application's startup, before building a window. Give each window the "neo"
 * CSS class (neo_window_new does). Widgets that draw themselves take their colours from neo_color (); everything
 * styled with CSS uses the same palette under the names @neo_primary, @neo_on_surface, and so on. */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* The Material 3 colour roles, dark. The values are the reference phone's dynamic palette. */
typedef enum {
  NEO_PRIMARY,
  NEO_ON_PRIMARY,
  NEO_PRIMARY_CONTAINER,
  NEO_ON_PRIMARY_CONTAINER,
  NEO_INVERSE_PRIMARY,
  NEO_SECONDARY,
  NEO_ON_SECONDARY,
  NEO_SECONDARY_CONTAINER,
  NEO_ON_SECONDARY_CONTAINER,
  NEO_TERTIARY,
  NEO_ON_TERTIARY,
  NEO_TERTIARY_CONTAINER,
  NEO_ON_TERTIARY_CONTAINER,
  NEO_ERROR,
  NEO_ERROR_CONTAINER,
  NEO_SURFACE,
  NEO_SURFACE_CONTAINER_LOWEST,
  NEO_SURFACE_CONTAINER_LOW,
  NEO_SURFACE_CONTAINER,
  NEO_SURFACE_CONTAINER_HIGH,
  NEO_SURFACE_CONTAINER_HIGHEST,
  NEO_SURFACE_BRIGHT,
  NEO_ON_SURFACE,
  NEO_ON_SURFACE_VARIANT,
  NEO_OUTLINE,
  NEO_OUTLINE_VARIANT,
  NEO_INVERSE_SURFACE,
  NEO_WHITE,
  NEO_N_COLORS
} NeoColor;

/* Loads the stylesheet (with the palette) for the whole display. Safe to call more than once. */
void    neo_init        (void);

/* Whether the optional Renaissance theme (setting de.yesman.GnomePhone.Theme name) is in force. It follows the setting
 * live: the CSS reloads, drawn widgets recolour on their next draw. */
gboolean neo_theme_is_renaissance (void);
/* A colour an app brings of its own, as the Renaissance look wants it (unchanged in the stock look): teal and cyan
 * pastels to gold, dark teal to lapis, neutrals to the night, white to parchment. See neo-pressable fills. */
GdkRGBA  neo_theme_map_rgba (const GdkRGBA *color);

GdkRGBA neo_color       (NeoColor color);
GdkRGBA neo_color_alpha (NeoColor color, float alpha);
/* a + (b - a) * t, per channel: the colour blends Android's animators do (ArgbEvaluator, blendARGB) */
GdkRGBA neo_rgba_mix    (const GdkRGBA *a, const GdkRGBA *b, float t);

/* A window with the "neo" class, its content on a root overlay that dialogs and snackbars can cover
 * (neo_window_get_overlay). With NEO_SHOT=file.png set, it renders itself to that file once it shows and on each
 * SIGUSR1: a picture of the UI for testing, made offscreen, so it works behind the lock screen too. */
GtkWidget  *neo_window_new         (GtkApplication *app);
void        neo_window_set_content (GtkWindow *window, GtkWidget *content);
/* the root overlay of the window `widget` is in, or NULL */
GtkOverlay *neo_window_get_overlay (GtkWidget *widget);

/* Device pixels per dp for `widget`: its scale factor on the phone, or NEO_SHOT_SCALE on a test display at scale 1,
 * so that what Android does in whole pixels (sizes, guidelines, centring) comes out the same in the test's picture */
int     neo_device_scale (GtkWidget *widget);

/* Android's includeFontPadding (on by default in every TextView): the line is as tall as the font's bounding box
 * (Roboto's yMax 2163 and yMin -555 of 2048 units) rather than its ascent and descent (1900, -500), so a text view
 * stands 0.128 of the text size taller above the text and 0.027 below it than a GtkLabel does. This adds those
 * margins to a label whose text is `size` px, for layouts measured against Android, and pins the text right under
 * the top one (yalign 0), where Android draws it. Centre such a label in its row with valign, as Android centres
 * the text view, not with yalign: GtkLabel rounds a centred line down to the whole pixel above, up to 1 dp too high. */
void    neo_label_set_font_padding (GtkWidget *label, double size);

/* A symbolic icon from the icon theme, drawn at `size` dp in `color` (an Android icon with its tint) */
void    neo_snapshot_icon (GtkSnapshot *snapshot, GtkWidget *widget, const char *icon_name, double x, double y,
                           double size, const GdkRGBA *color);

G_END_DECLS
