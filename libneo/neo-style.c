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

#include "neo-looks.h"   /* generated from tools/theme/palette.json: the four looks' role tables and extras */

/* "Cyberpunk meets Italian Renaissance", the optional look (setting de.yesman.GnomePhone.Theme name=renaissance), in
 * four variants: Giorno Marmo and Marino (light), Notte Bosco and Prugna (dark). The effective one is the Theme's
 * night-look when the system style (org.gnome.desktop.interface color-scheme) is prefer-dark, else its day-look. Same
 * roles, other values, so every widget follows; neo-renaissance.css adds the ornament, from the look's extras (the
 * gold, silver, card and scrim colours: @neo_gold ...). Michelangelus (not shipped) is named in that file with
 * fallbacks. */
static const NeoLook   *look;            /* the look in force, NULL in the stock look */
static GtkCssProvider  *provider, *theme_provider;
static GSettings       *theme_settings, *iface_settings;

static guint32
rgb_of (NeoColor color)
{
  return look ? look->role[color] : palette[color].rgb;
}

gboolean
neo_theme_is_renaissance (void)
{
  return look != NULL;
}

const char *
neo_theme_look (void)
{
  return look ? look->id : NULL;
}

gboolean
neo_theme_is_day (void)
{
  return look && look->day;
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
  if (look)
    {
      /* the look's extras (palette.json), for neo-renaissance.css: colour names are per sheet, so every sheet that
       * names them gets the definitions */
      const struct { const char *css; guint32 rgb; } extra[] = {
        { "neo_ink", look->ink }, { "neo_gold", look->gold }, { "neo_gold_hi", look->gold_hi },
        { "neo_accent", look->accent }, { "neo_silver", look->silver }, { "neo_silver_dim", look->silver_dim },
        { "neo_wall_a", look->wall_a }, { "neo_wall_b", look->wall_b }, { "neo_wall_vein", look->wall_vein },
        { "neo_card", look->card }, { "neo_line", look->line }, { "neo_hover", look->hover },
        { "neo_scrim", look->scrim }, { "neo_chisel_lit", look->chisel_lit },
        { "neo_chisel_dark", look->chisel_dark }, { "neo_amber", look->glow }, { "neo_lapis", look->lapis },
      };
      for (guint i = 0; i < G_N_ELEMENTS (extra); i++)
        g_string_append_printf (css, "@define-color %s #%06x;\n", extra[i].css, extra[i].rgb);
      g_string_append_printf (css, "@define-color neo_card_a %s;\n@define-color neo_rim %s;\n"
                                   "@define-color neo_cardsh %s;\n",
                              look->card_a, look->rim, look->cardsh);
    }
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
 * apps that carry their own (neo-renaissance-apps-day.css or -night.css, generated, by the look's scheme) */
static void
load_css (void)
{
  GString *css = palette_css ();
  add_sheet (css, "/de/yesman/neo/neo.css");
  gtk_css_provider_load_from_string (provider, css->str);
  g_string_free (css, TRUE);

  css = palette_css ();
  if (look)
    {
      add_sheet (css, look->day ? "/de/yesman/neo/neo-renaissance-apps-day.css"
                                : "/de/yesman/neo/neo-renaissance-apps-night.css");
      add_sheet (css, "/de/yesman/neo/neo-renaissance.css");
      if (theme_settings)
        fonts_css (css);
    }
  gtk_css_provider_load_from_string (theme_provider, css->str);
  g_string_free (css, TRUE);
}

/* a key of the theme setting, when the schema has it (a phone with an older schema has no day-look and night-look) */
static char *
theme_string (const char *key)
{
  GSettingsSchema *schema = NULL;
  char *value = NULL;

  g_object_get (theme_settings, "settings-schema", &schema, NULL);
  if (schema && g_settings_schema_has_key (schema, key))
    value = g_settings_get_string (theme_settings, key);
  g_clear_pointer (&schema, g_settings_schema_unref);
  return value;
}

/* The look in force: none while the setting is not 'renaissance' (or its schema is not installed: such a phone stays on
 * the stock look); else the night-look when the system style is dark, the day-look when it is not. An older schema
 * without the two keys has the one look of v3, plum, at night, and marble by day. */
static const NeoLook *
look_wanted (void)
{
  g_autofree char *name = theme_settings ? g_settings_get_string (theme_settings, "name") : NULL;
  if (g_strcmp0 (name, "renaissance") != 0)
    return NULL;
  gboolean dark = iface_settings && g_settings_get_enum (iface_settings, "color-scheme") == 1;   /* prefer-dark */
  g_autofree char *id = theme_string (dark ? "night-look" : "day-look");
  if (!id)
    id = g_strdup (dark ? "plum" : "marble");
  for (guint i = 0; i < G_N_ELEMENTS (neo_looks); i++)
    if (g_strcmp0 (neo_looks[i].id, id) == 0)
      return &neo_looks[i];
  for (guint i = 0; i < G_N_ELEMENTS (neo_looks); i++)
    if (neo_looks[i].day == !dark)
      return &neo_looks[i];
  return NULL;
}

/* GTK's own scheme for the widgets libneo leaves to the theme (scrollbars, popovers, menus): dark in the stock look and
 * in the night looks (the apps are always dark there, whatever the system's preference: Theme.DeskClock's parent is a
 * Dark theme), light in the day looks */
static void
apply_gtk_scheme (void)
{
  gboolean day = look && look->day;
#if GTK_CHECK_VERSION (4, 20, 0)
  g_object_set (gtk_settings_get_default (), "gtk-interface-color-scheme",
                day ? GTK_INTERFACE_COLOR_SCHEME_LIGHT : GTK_INTERFACE_COLOR_SCHEME_DARK, NULL);
#else
  g_object_set (gtk_settings_get_default (), "gtk-application-prefer-dark-theme", !day, NULL);
#endif
}

/* the look's classes on a neo window: neo-look-day or neo-look-night, and neo-look-<id>; none in the stock look */
static void
mark_window (GtkWidget *window)
{
  static const char *const classes[] = { "neo-look-day", "neo-look-night", "neo-look-marble", "neo-look-sea",
                                         "neo-look-forest", "neo-look-plum" };
  for (guint i = 0; i < G_N_ELEMENTS (classes); i++)
    gtk_widget_remove_css_class (window, classes[i]);
  if (!look)
    return;
  g_autofree char *id = g_strconcat ("neo-look-", look->id, NULL);
  gtk_widget_add_css_class (window, look->day ? "neo-look-day" : "neo-look-night");
  gtk_widget_add_css_class (window, id);
}

/* the theme setting or the system style changed: the look is worked out again, and everything follows it, live */
static void
on_theme_changed (GSettings *settings, const char *key, gpointer data)
{
  const NeoLook *want = look_wanted ();
  /* a font chain changed while the look is on: reload the sheet, the widgets re-lay out on their own */
  if (want == look && !(look && g_str_has_prefix (key, "font-")))
    return;
  look = want;
  apply_gtk_scheme ();
  load_css ();
  /* widgets that draw with neo_color () pick the new values on their next draw */
  GListModel *windows = gtk_window_get_toplevels ();
  for (guint i = 0; i < g_list_model_get_n_items (windows); i++)
    {
      GtkWidget *w = g_list_model_get_item (windows, i);
      if (gtk_widget_has_css_class (w, "neo"))
        mark_window (w);
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
  if (!look)
    return *c;
  double h, l, s;
  GdkRGBA out = { 0, 0, 0, c->alpha };
  rgb_to_hls (c->red, c->green, c->blue, &h, &l, &s);
  double deg = h * 360;
  if (look->day)
    {
      /* by day the apps' dark surfaces turn light and their light text dark (same hue families): neutrals invert
       * their lightness, dark teal (bars, containers) turns pale gold, pastel teal (FABs, accents) turns deep gold */
      if (s < 0.14)
        {
          if (l > 0.93)
            out = (GdkRGBA) { 0x2b / 255., 0x2c / 255., 0x31 / 255., c->alpha };
          else if (l > 0.75)
            hls_to_rgb (225. / 360, 0.2 + (1 - l) * 0.9, 0.08, &out);
          else if (l < 0.45)
            hls_to_rgb (40. / 360, 0.97 - l * 0.45, 0.10, &out);
          else
            hls_to_rgb (225. / 360, l * 0.85, 0.06, &out);
        }
      else if (deg >= 140 && deg <= 230 && s < 0.95)
        {
          if (l < 0.42)
            hls_to_rgb (41. / 360, 0.97 - l * 0.45, 0.45, &out);
          else
            hls_to_rgb (38. / 360, 0.62 - (MIN (l, 0.78) - 0.42) * 0.55, 0.52, &out);
        }
      else
        return *c;
      out.alpha = c->alpha;
      return out;
    }
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
  GSettingsSchema *iface = source ? g_settings_schema_source_lookup (source, "org.gnome.desktop.interface", TRUE) : NULL;
  if (iface && g_settings_schema_has_key (iface, "color-scheme"))
    {
      iface_settings = g_settings_new_full (iface, NULL, NULL);
      g_signal_connect (iface_settings, "changed::color-scheme", G_CALLBACK (on_theme_changed), NULL);
    }
  g_clear_pointer (&iface, g_settings_schema_unref);
  if (schema)
    {
      theme_settings = g_settings_new_full (schema, NULL, NULL);
      g_settings_schema_unref (schema);
      g_signal_connect (theme_settings, "changed::name", G_CALLBACK (on_theme_changed), NULL);
      g_signal_connect (theme_settings, "changed::day-look", G_CALLBACK (on_theme_changed), NULL);
      g_signal_connect (theme_settings, "changed::night-look", G_CALLBACK (on_theme_changed), NULL);
      g_signal_connect (theme_settings, "changed::font-display", G_CALLBACK (on_theme_changed), NULL);
      g_signal_connect (theme_settings, "changed::font-body", G_CALLBACK (on_theme_changed), NULL);
      g_signal_connect (theme_settings, "changed::font-mono", G_CALLBACK (on_theme_changed), NULL);
      look = look_wanted ();
    }

  provider = gtk_css_provider_new ();
  theme_provider = gtk_css_provider_new ();
  load_css ();
  /* above the theme (APPLICATION), so GTK's own look never shows through a libneo widget */
  gtk_style_context_add_provider_for_display (gdk_display_get_default (), GTK_STYLE_PROVIDER (provider),
                                              GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  gtk_style_context_add_provider_for_display (gdk_display_get_default (), GTK_STYLE_PROVIDER (theme_provider),
                                              GTK_STYLE_PROVIDER_PRIORITY_USER);

  apply_gtk_scheme ();
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
  mark_window (window);
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
