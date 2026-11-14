/* neo-style: the palette, the stylesheet and the window frame. See neo-style.h. */
#include "neo-style.h"

#include <glib-unix.h>
#include <math.h>
#include <signal.h>
#include <stdlib.h>

/* The reference phone's dark palette (andrec/phone-apps/overlay-dynamic.txt, the system_*_dark values). The CSS
 * name of each role is its enum name in lower case: NEO_ON_SURFACE is @neo_on_surface. */
static const struct {
  const char *css;
  guint32     rgb;
} palette[NEO_N_COLORS] = {
  [NEO_PRIMARY]                   = { "neo_primary",                   0x9ccfd7 },
  [NEO_ON_PRIMARY]                = { "neo_on_primary",                0x0d474e },
  [NEO_PRIMARY_CONTAINER]         = { "neo_primary_container",         0x265a61 },
  [NEO_ON_PRIMARY_CONTAINER]      = { "neo_on_primary_container",      0xb9ecf4 },
  [NEO_INVERSE_PRIMARY]           = { "neo_inverse_primary",           0x34666d },
  [NEO_SECONDARY]                 = { "neo_secondary",                 0xb1cbcf },
  [NEO_ON_SECONDARY]              = { "neo_on_secondary",              0x2c4448 },
  [NEO_SECONDARY_CONTAINER]       = { "neo_secondary_container",       0x273f43 },
  [NEO_ON_SECONDARY_CONTAINER]    = { "neo_on_secondary_container",    0xaac4c8 },
  [NEO_TERTIARY]                  = { "neo_tertiary",                  0xc9e2ff },
  [NEO_ON_TERTIARY]               = { "neo_on_tertiary",               0x315371 },
  [NEO_TERTIARY_CONTAINER]        = { "neo_tertiary_container",        0xb3d5f9 },
  [NEO_ON_TERTIARY_CONTAINER]     = { "neo_on_tertiary_container",     0x284a67 },
  [NEO_ERROR]                     = { "neo_error",                     0xfa746f },
  [NEO_ERROR_CONTAINER]           = { "neo_error_container",           0x871f21 },
  [NEO_SURFACE]                   = { "neo_surface",                   0x0a0f10 },
  [NEO_SURFACE_CONTAINER_LOWEST]  = { "neo_surface_container_lowest",  0x000000 },
  [NEO_SURFACE_CONTAINER_LOW]     = { "neo_surface_container_low",     0x0e1415 },
  [NEO_SURFACE_CONTAINER]         = { "neo_surface_container",         0x131b1c },
  [NEO_SURFACE_CONTAINER_HIGH]    = { "neo_surface_container_high",    0x192122 },
  [NEO_SURFACE_CONTAINER_HIGHEST] = { "neo_surface_container_highest", 0x1e2729 },
  [NEO_SURFACE_BRIGHT]            = { "neo_surface_bright",            0x242e2f },
  [NEO_ON_SURFACE]                = { "neo_on_surface",                0xdde7e9 },
  [NEO_ON_SURFACE_VARIANT]        = { "neo_on_surface_variant",        0xa2adaf },
  [NEO_OUTLINE]                   = { "neo_outline",                   0x6d7779 },
  [NEO_OUTLINE_VARIANT]           = { "neo_outline_variant",           0x404a4b },
  [NEO_INVERSE_SURFACE]           = { "neo_inverse_surface",           0xf6fafb },
  [NEO_WHITE]                     = { "neo_white",                     0xffffff },
};

/* "Cyberpunk meets Italian Renaissance", the optional look (setting de.yesman.GnomePhone.Theme name=renaissance):
 * an OLED night of lapis-tinted black, gold leaf as the accent, terracotta for the second one, parchment text and
 * amber (the camera eye) as the only warm edge light. Same roles, other values, so every widget follows; neo-renaissance.css adds the
 * ornament. Michelangelus (not shipped) is named in that file with fallbacks. */
static const guint32 renaissance_rgb[NEO_N_COLORS] = {
  [NEO_PRIMARY]                   = 0xd9b26a,
  [NEO_ON_PRIMARY]                = 0x2a1d04,
  [NEO_PRIMARY_CONTAINER]         = 0x26242f,   /* lapis only as a small accent (neo_lapis), fills stay dark */
  [NEO_ON_PRIMARY_CONTAINER]      = 0xf3dc9f,
  [NEO_INVERSE_PRIMARY]           = 0x8a6a2c,
  [NEO_SECONDARY]                 = 0xe0936f,
  [NEO_ON_SECONDARY]              = 0x3d1a0b,
  [NEO_SECONDARY_CONTAINER]       = 0x4a2a1c,
  [NEO_ON_SECONDARY_CONTAINER]    = 0xf6cdb8,
  [NEO_TERTIARY]                  = 0xff9f2e,
  [NEO_ON_TERTIARY]               = 0x00343c,
  [NEO_TERTIARY_CONTAINER]        = 0x0a4650,
  [NEO_ON_TERTIARY_CONTAINER]     = 0xb2f5ff,
  [NEO_ERROR]                     = 0xff6f9c,
  [NEO_ERROR_CONTAINER]           = 0x6b1237,
  [NEO_SURFACE]                   = 0x08070d,
  [NEO_SURFACE_CONTAINER_LOWEST]  = 0x000000,
  [NEO_SURFACE_CONTAINER_LOW]     = 0x0d0c15,
  [NEO_SURFACE_CONTAINER]         = 0x13121e,
  [NEO_SURFACE_CONTAINER_HIGH]    = 0x1a1927,
  [NEO_SURFACE_CONTAINER_HIGHEST] = 0x211f31,
  [NEO_SURFACE_BRIGHT]            = 0x2a283d,
  [NEO_ON_SURFACE]                = 0xf0e6d0,
  [NEO_ON_SURFACE_VARIANT]        = 0xbdb299,
  [NEO_OUTLINE]                   = 0x6e7280,   /* silver-dim: tools/theme/palette.json */
  [NEO_OUTLINE_VARIANT]           = 0x3a3d4a,
  [NEO_INVERSE_SURFACE]           = 0xf3ead7,
  [NEO_WHITE]                     = 0xffffff,
};

static gboolean       renaissance;       /* the theme in force */
static GtkCssProvider *provider, *theme_provider;
static GSettings      *theme_settings;

static guint32
rgb_of (NeoColor color)
{
  return renaissance ? renaissance_rgb[color] : palette[color].rgb;
}

gboolean
neo_theme_is_renaissance (void)
{
  return renaissance;
}

GdkRGBA
neo_color (NeoColor color)
{
  return neo_color_alpha (color, 1);
}

GdkRGBA
neo_color_alpha (NeoColor color, float alpha)
{
  guint32 rgb = rgb_of (color);
  return (GdkRGBA) { ((rgb >> 16) & 0xff) / 255.f, ((rgb >> 8) & 0xff) / 255.f, (rgb & 0xff) / 255.f, alpha };
}

GdkRGBA
neo_rgba_mix (const GdkRGBA *a, const GdkRGBA *b, float t)
{
  return (GdkRGBA) {
    a->red + (b->red - a->red) * t,
    a->green + (b->green - a->green) * t,
    a->blue + (b->blue - a->blue) * t,
    a->alpha + (b->alpha - a->alpha) * t,
  };
}

static void
add_sheet (GString *css, const char *resource)
{
  GBytes *sheet = g_resources_lookup_data (resource, 0, NULL);
  if (!sheet)
    return;
  g_string_append_len (css, g_bytes_get_data (sheet, NULL), g_bytes_get_size (sheet));
  g_string_append_c (css, '\n');
  g_bytes_unref (sheet);
}

static GString *
palette_css (void)
{
  GString *css = g_string_new (NULL);
  for (int i = 0; i < NEO_N_COLORS; i++)
    g_string_append_printf (css, "@define-color %s #%06x;\n", palette[i].css, rgb_of (i));
  return css;
}

/* "A, B, C" from the setting, as quoted CSS families, then the stock fallback; the stock fallback alone when empty */
static void
append_families (GString *css, const char *chain, const char *fallback)
{
  if (chain)
    {
      g_auto (GStrv) names = g_strsplit (chain, ",", -1);
      for (int i = 0; names[i]; i++)
        {
          g_strstrip (names[i]);
          g_strdelimit (names[i], "\"'\\;{}", ' ');
          g_strstrip (names[i]);
          if (*names[i])
            g_string_append_printf (css, "\"%s\", ", names[i]);
        }
    }
  g_string_append (css, fallback);
}

/* the Renaissance type: the families of the three roles (settings font-display, font-body, font-mono) */
static void
fonts_css (GString *css)
{
  struct { const char *key, *selectors, *fallback; } role[] = {
    { "font-body", "window.neo", "\"Neo Roboto\", Roboto, sans-serif" },
    { "font-display", ".neo-display-large, .neo-headline-large, .neo-title-large, .neo-app-bar-title, .neo-dialog-title",
      "\"Neo Roboto\", serif" },
    { "font-mono", ".neo-tabular", "monospace" },
  };
  GSettingsSchema *schema = NULL;
  g_object_get (theme_settings, "settings-schema", &schema, NULL);
  for (guint i = 0; i < G_N_ELEMENTS (role); i++)
    {
      g_autofree char *chain = schema && g_settings_schema_has_key (schema, role[i].key)
                                 ? g_settings_get_string (theme_settings, role[i].key) : NULL;
      g_string_append_printf (css, "%s { font-family: ", role[i].selectors);
      append_families (css, chain, role[i].fallback);
      g_string_append (css, "; }\n");
    }
  g_clear_pointer (&schema, g_settings_schema_unref);
}

/* the palette as @define-color lines, then neo.css. The Renaissance theme is a second sheet above the apps' own (USER
 * priority, with the palette again: colour names are per sheet): the ornament, and the recoloured palettes of the
 * apps that carry their own (neo-renaissance-apps.css, generated) */
static void
load_css (void)
{
  GString *css = palette_css ();
  add_sheet (css, "/de/yesman/neo/neo.css");
  gtk_css_provider_load_from_string (provider, css->str);
  g_string_free (css, TRUE);

  css = palette_css ();
  if (renaissance)
    {
      add_sheet (css, "/de/yesman/neo/neo-renaissance-apps.css");
      add_sheet (css, "/de/yesman/neo/neo-renaissance.css");
      if (theme_settings)
        fonts_css (css);
    }
  gtk_css_provider_load_from_string (theme_provider, css->str);
  g_string_free (css, TRUE);
}

/* the setting, when its schema is installed (a phone without the theme setting stays on the stock look) */
static gboolean
theme_wanted (void)
{
  if (!theme_settings)
    return FALSE;
  g_autofree char *name = g_settings_get_string (theme_settings, "name");
  return g_strcmp0 (name, "renaissance") == 0;
}

static void
on_theme_changed (GSettings *settings, const char *key, gpointer data)
{
  gboolean want = theme_wanted ();
  /* a font chain changed while the look is on: reload the sheet, the widgets re-lay out on their own */
  if (want == renaissance && !(renaissance && g_str_has_prefix (key, "font-")))
    return;
  renaissance = want;
  load_css ();
  /* widgets that draw with neo_color () pick the new values on their next draw */
  GListModel *windows = gtk_window_get_toplevels ();
  for (guint i = 0; i < g_list_model_get_n_items (windows); i++)
    {
      GtkWidget *w = g_list_model_get_item (windows, i);
      gtk_widget_queue_draw (w);
      g_object_unref (w);
    }
}

/* The Renaissance look for the apps that carry colours of their own (Messaging's #b8ebf3 FAB, Dialer's teal ...): the
 * same mapping as tools/theme/gen_app_overrides.py does for their CSS. Teal and cyan pastels go gold, dark teal goes
 * lapis, neutrals take the night's violet cast, white goes to parchment; other hues stay. Identity in the stock look. */
static void
rgb_to_hls (double r, double g, double b, double *h, double *l, double *s)
{
  double max = MAX (r, MAX (g, b)), min = MIN (r, MIN (g, b)), d = max - min;
  *l = (max + min) / 2;
  *h = 0;
  *s = 0;
  if (d == 0)
    return;
  *s = *l > 0.5 ? d / (2 - max - min) : d / (max + min);
  if (max == r)
    *h = fmod ((g - b) / d + (g < b ? 6 : 0), 6) / 6;
  else if (max == g)
    *h = ((b - r) / d + 2) / 6;
  else
    *h = ((r - g) / d + 4) / 6;
}

static double
hue_channel (double p, double q, double t)
{
  if (t < 0) t += 1;
  if (t > 1) t -= 1;
  if (t < 1. / 6) return p + (q - p) * 6 * t;
  if (t < 1. / 2) return q;
  if (t < 2. / 3) return p + (q - p) * (2. / 3 - t) * 6;
  return p;
}

static void
hls_to_rgb (double h, double l, double s, GdkRGBA *out)
{
  if (s == 0)
    {
      out->red = out->green = out->blue = l;
      return;
    }
  double q = l < 0.5 ? l * (1 + s) : l + s - l * s, p = 2 * l - q;
  out->red = hue_channel (p, q, h + 1. / 3);
  out->green = hue_channel (p, q, h);
  out->blue = hue_channel (p, q, h - 1. / 3);
}

GdkRGBA
neo_theme_map_rgba (const GdkRGBA *c)
{
  if (!renaissance)
    return *c;
  double h, l, s;
  GdkRGBA out = { 0, 0, 0, c->alpha };
  rgb_to_hls (c->red, c->green, c->blue, &h, &l, &s);
  double deg = h * 360;
  if (s < 0.14)
    {
      if (l > 0.93)
        out = (GdkRGBA) { 0xf0 / 255., 0xe6 / 255., 0xd0 / 255., c->alpha };
      else if (l > 0.75)
        hls_to_rgb (41. / 360, l * 0.97, 0.22, &out);
      else if (l < 0.45)
        hls_to_rgb (250. / 360, l * 0.55, 0.28, &out);
      else
        hls_to_rgb (41. / 360, l * 0.9, 0.10, &out);
    }
  else if (deg >= 140 && deg <= 230 && s < 0.95)
    {
      if (l < 0.42)
        hls_to_rgb (224. / 360, l * 0.85, 0.52, &out);
      else
        hls_to_rgb (41. / 360, MIN (l, 0.78), 0.55, &out);
    }
  else
    return *c;
  out.alpha = c->alpha;
  return out;
}

void
neo_init (void)
{
  static gboolean done;
  if (done)
    return;
  done = TRUE;

  GSettingsSchemaSource *source = g_settings_schema_source_get_default ();
  GSettingsSchema *schema = source ? g_settings_schema_source_lookup (source, "de.yesman.GnomePhone.Theme", TRUE) : NULL;
  if (schema)
    {
      theme_settings = g_settings_new_full (schema, NULL, NULL);
      g_settings_schema_unref (schema);
      g_signal_connect (theme_settings, "changed::name", G_CALLBACK (on_theme_changed), NULL);
      g_signal_connect (theme_settings, "changed::font-display", G_CALLBACK (on_theme_changed), NULL);
      g_signal_connect (theme_settings, "changed::font-body", G_CALLBACK (on_theme_changed), NULL);
      g_signal_connect (theme_settings, "changed::font-mono", G_CALLBACK (on_theme_changed), NULL);
      renaissance = theme_wanted ();
    }

  provider = gtk_css_provider_new ();
  theme_provider = gtk_css_provider_new ();
  load_css ();
  /* above the theme (APPLICATION), so GTK's own look never shows through a libneo widget */
  gtk_style_context_add_provider_for_display (gdk_display_get_default (), GTK_STYLE_PROVIDER (provider),
                                              GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  gtk_style_context_add_provider_for_display (gdk_display_get_default (), GTK_STYLE_PROVIDER (theme_provider),
                                              GTK_STYLE_PROVIDER_PRIORITY_USER);

  /* the apps are always dark, whatever the system's preference (Theme.DeskClock's parent is a Dark theme) */
#if GTK_CHECK_VERSION (4, 20, 0)
  g_object_set (gtk_settings_get_default (), "gtk-interface-color-scheme", GTK_INTERFACE_COLOR_SCHEME_DARK, NULL);
#else
  g_object_set (gtk_settings_get_default (), "gtk-application-prefer-dark-theme", TRUE, NULL);
#endif
}

/* ---- the window ------------------------------------------------------------------------------------------------ */

/* ---- NEO_SHOT: a window's picture, for checking the UI ---------------------------------------------------------
 *
 * With NEO_SHOT=/path/shot.png in the environment, a window renders itself to that file, at the screen's scale, a
 * second after it maps, and again on every SIGUSR1 (after the test has changed something). The picture is drawn
 * offscreen from the widgets, so it works where a screenshot cannot: behind the lock screen, on a phone nobody
 * unlocks for the test. (A window hidden there gets no frames, though, so its animations stop where they are: for
 * those, run the app on a display of its own, such as a headless compositor, with NEO_SHOT_SCALE=3.) */

static gboolean
shoot (gpointer data)
{
  GtkWidget *window = data;
  const char *path = g_getenv ("NEO_SHOT");
  int width = gtk_widget_get_width (window), height = gtk_widget_get_height (window);
  /* the screen's scale, or NEO_SHOT_SCALE: a window on a test display at scale 1 still makes a phone-sized picture */
  int scale = neo_device_scale (window);
  GdkPaintable *paintable = gtk_widget_paintable_new (window);
  GtkSnapshot *snapshot = gtk_snapshot_new ();
  GskRenderNode *node;

  gtk_snapshot_scale (snapshot, scale, scale);
  gdk_paintable_snapshot (paintable, snapshot, width, height);
  node = gtk_snapshot_free_to_node (snapshot);
  if (!gtk_widget_get_mapped (window) || !node)
    g_message ("NEO_SHOT: nothing to draw (the window is %s)", gtk_widget_get_mapped (window) ? "empty" : "not shown");
  else
    {
      GdkTexture *texture = gsk_renderer_render_texture (gtk_native_get_renderer (GTK_NATIVE (window)), node,
                                                         &GRAPHENE_RECT_INIT (0, 0, width * scale, height * scale));
      if (gdk_texture_save_to_png (texture, path))
        g_message ("NEO_SHOT: %s, %d×%d", path, width * scale, height * scale);
      else
        g_message ("NEO_SHOT: cannot write %s", path);
      g_object_unref (texture);
    }
  g_clear_pointer (&node, gsk_render_node_unref);
  g_object_unref (paintable);
  return G_SOURCE_REMOVE;
}

static void
on_shot_map (GtkWidget *window)
{
  g_timeout_add_full (G_PRIORITY_DEFAULT, 1000, shoot, g_object_ref (window), g_object_unref);
}

static gboolean
on_shot_signal (gpointer data)
{
  GListModel *windows = gtk_window_get_toplevels ();

  for (guint i = 0; i < g_list_model_get_n_items (windows); i++)
    {
      g_autoptr(GtkWidget) window = g_list_model_get_item (windows, i);

      if (gtk_widget_has_css_class (window, "neo"))
        shoot (window);
    }
  return G_SOURCE_CONTINUE;
}

GtkWidget *
neo_window_new (GtkApplication *app)
{
  static gboolean shots;
  GtkWidget *window = gtk_application_window_new (app);
  gtk_widget_add_css_class (window, "neo");
  /* no title bar: the app draws its own 64 dp app bar, and on the phone the shell shows every window full screen */
  gtk_window_set_decorated (GTK_WINDOW (window), FALSE);
  gtk_window_set_default_size (GTK_WINDOW (window), 360, 760);
  if (g_getenv ("NEO_SHOT"))
    {
      g_signal_connect (window, "map", G_CALLBACK (on_shot_map), NULL);
      if (!shots)
        g_unix_signal_add (SIGUSR1, on_shot_signal, NULL);
      shots = TRUE;
    }
  return window;
}

void
neo_window_set_content (GtkWindow *window, GtkWidget *content)
{
  GtkWidget *overlay = gtk_overlay_new ();
  gtk_overlay_set_child (GTK_OVERLAY (overlay), content);
  g_object_set_data (G_OBJECT (window), "neo-overlay", overlay);
  gtk_window_set_child (window, overlay);
}

GtkOverlay *
neo_window_get_overlay (GtkWidget *widget)
{
  GtkRoot *root = gtk_widget_get_root (widget);
  return root ? g_object_get_data (G_OBJECT (root), "neo-overlay") : NULL;
}

int
neo_device_scale (GtkWidget *widget)
{
  const char *shot_scale = g_getenv ("NEO_SHOT_SCALE");

  return shot_scale && atoi (shot_scale) > 0 ? atoi (shot_scale) : gtk_widget_get_scale_factor (widget);
}

/* ---- text ---------------------------------------------------------------------------------------------------- */

void
neo_label_set_font_padding (GtkWidget *label, double size)
{
  gtk_widget_set_margin_top (label, (int) round (size * (2163 - 1900) / 2048.0));
  gtk_widget_set_margin_bottom (label, (int) round (size * (555 - 500) / 2048.0));
  gtk_label_set_yalign (GTK_LABEL (label), 0);
}

/* ---- icons ----------------------------------------------------------------------------------------------------- */

void
neo_snapshot_icon (GtkSnapshot *snapshot, GtkWidget *widget, const char *icon_name, double x, double y, double size,
                   const GdkRGBA *color)
{
  GtkIconTheme *theme = gtk_icon_theme_get_for_display (gtk_widget_get_display (widget));
  GtkIconPaintable *icon = gtk_icon_theme_lookup_icon (theme, icon_name, NULL, (int) size,
                                                       gtk_widget_get_scale_factor (widget), GTK_TEXT_DIR_NONE, 0);
  gtk_snapshot_save (snapshot);
  gtk_snapshot_translate (snapshot, &GRAPHENE_POINT_INIT (x, y));
  /* the symbolic colours are foreground, error, warning, success: an Android tint is the first of them */
  GdkRGBA colors[4] = { *color, *color, *color, *color };
  gtk_symbolic_paintable_snapshot_symbolic (GTK_SYMBOLIC_PAINTABLE (icon), snapshot, size, size, colors, 4);
  gtk_snapshot_restore (snapshot);
  g_object_unref (icon);
}
