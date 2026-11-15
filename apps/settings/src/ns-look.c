/* ns-look: the Renaissance look of Neo Settings in its four variants. See ns-look.h. */
#include "ns-look.h"
#include "ns-ui.h"

#define RES "/de/yesman/NeoSettings/"

typedef struct {
  const char *id;
  gboolean    day;
} Look;

static const Look looks[] = { { "marble", TRUE }, { "sea", TRUE }, { "forest", FALSE }, { "plum", FALSE } };

static const Look   *look;                 /* in force, NULL in the stock look */
static GSettings    *theme, *iface;
static GtkCssProvider *css;
static GtkWidget    *window;
static GHashTable   *tracked;              /* the GtkImages that follow the look: image -> NULL */
static GHashTable   *textures;             /* glyph path -> GdkTexture (NULL when unreadable) */
static char         *pack_dir[2];          /* the glyph packs' icons dirs: [day], [night]; "" when not found */

/* a string key of the theme setting, when the installed schema has it (an older one has no day-look/night-look) */
static char *
theme_string (const char *key)
{
  return theme && ns_has_key (theme, key) ? g_settings_get_string (theme, key) : NULL;
}

/* the same rule as libneo's neo_theme_look (): the setting's night-look in a dark system style, else its day-look */
static const Look *
look_wanted (void)
{
  g_autofree char *name = theme ? g_settings_get_string (theme, "name") : NULL;
  if (g_strcmp0 (name, "renaissance") != 0)
    return NULL;
  gboolean dark = iface && g_settings_get_enum (iface, "color-scheme") == 1;   /* prefer-dark */
  g_autofree char *id = theme_string (dark ? "night-look" : "day-look");
  for (guint i = 0; id && i < G_N_ELEMENTS (looks); i++)
    if (!g_strcmp0 (looks[i].id, id) && looks[i].day == !dark)
      return &looks[i];
  return dark ? &looks[3] : &looks[0];     /* the v3 look at night, marble by day */
}

const char *
ns_look (void)
{
  return look ? look->id : NULL;
}

gboolean
ns_look_is_day (void)
{
  return look && look->day;
}

/* the stylesheet: the tokens of the look in force (the stock look names plum's: the Renaissance rules never match
 * there, but every name they use must exist), then style.css. Colour names are per sheet, so they are one string. */
static void
load_css (void)
{
  g_autofree char *tokens = g_strconcat (RES "looks/ns-look-", look ? look->id : "plum", ".css", NULL);
  GString *sheet = g_string_new (NULL);
  const char *const parts[] = { tokens, RES "style.css" };

  for (guint i = 0; i < G_N_ELEMENTS (parts); i++) {
    GBytes *b = g_resources_lookup_data (parts[i], 0, NULL);
    if (b) {
      g_string_append_len (sheet, g_bytes_get_data (b, NULL), g_bytes_get_size (b));
      g_string_append_c (sheet, '\n');
      g_bytes_unref (b);
    }
  }
  gtk_css_provider_load_from_string (css, sheet->str);
  g_string_free (sheet, TRUE);
}

static void
mark_window (void)
{
  static const char *const classes[] = { "ns-ren", "ns-look-day", "ns-look-night", "ns-look-marble", "ns-look-sea",
                                         "ns-look-forest", "ns-look-plum" };
  for (guint i = 0; i < G_N_ELEMENTS (classes); i++)
    gtk_widget_remove_css_class (window, classes[i]);
  if (!look)
    return;
  g_autofree char *id = g_strconcat ("ns-look-", look->id, NULL);
  gtk_widget_add_css_class (window, "ns-ren");
  gtk_widget_add_css_class (window, look->day ? "ns-look-day" : "ns-look-night");
  gtk_widget_add_css_class (window, id);
}

/* ---- the glyph packs ---- */

/* neo-launcher/iconpacks/<pack>/icons in the user's data dir, then in each system data dir, as the launcher looks for
 * its packs (icons.c); "" when no data dir has it */
static const char *
pack_icons_dir (gboolean day)
{
  char **slot = &pack_dir[day ? 0 : 1];

  if (*slot)
    return *slot;
  const char *pack = day ? "renaissance-glyph-day" : "renaissance-glyph-night";
  g_autoptr (GStrvBuilder) dirs = g_strv_builder_new ();
  g_strv_builder_take (dirs, g_build_filename (g_get_user_data_dir (), "neo-launcher", "iconpacks", NULL));
  for (const char *const *d = g_get_system_data_dirs (); *d; d++)
    g_strv_builder_take (dirs, g_build_filename (*d, "neo-launcher", "iconpacks", NULL));
  g_auto (GStrv) all = g_strv_builder_end (dirs);
  *slot = g_strdup ("");
  for (int i = 0; all[i]; i++) {
    g_autofree char *dir = g_build_filename (all[i], pack, "icons", NULL);
    if (g_file_test (dir, G_FILE_TEST_IS_DIR)) {
      g_free (*slot);
      *slot = g_steal_pointer (&dir);
      break;
    }
  }
  return *slot;
}

/* the look's bare glyph for an icon name, or NULL (stock look, no pack installed, or no file of that name) */
static GdkTexture *
glyph_for (const char *icon_name)
{
  if (!look || !icon_name)
    return NULL;
  const char *dir = pack_icons_dir (look->day);
  if (!*dir)
    return NULL;
  g_autofree char *file = g_strconcat (icon_name, ".png", NULL);
  g_autofree char *path = g_build_filename (dir, file, NULL);
  if (!textures)
    textures = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_object_unref);
  GdkTexture *t = g_hash_table_lookup (textures, path);
  if (!t && g_file_test (path, G_FILE_TEST_IS_REGULAR)) {
    t = gdk_texture_new_from_filename (path, NULL);
    if (t)
      g_hash_table_insert (textures, g_strdup (path), t);
  }
  return t;
}

static void
apply_icon (GtkImage *image)
{
  const char *name = g_object_get_data (G_OBJECT (image), "ns-look-icon");
  GdkTexture *glyph = glyph_for (name);

  if (glyph) {
    gtk_image_set_from_paintable (image, GDK_PAINTABLE (glyph));
    gtk_image_set_pixel_size (image, 32);
    gtk_widget_add_css_class (GTK_WIDGET (image), "ns-glyph");
  } else {
    gtk_image_set_from_icon_name (image, name);
    gtk_image_set_pixel_size (image, 20);
    gtk_widget_remove_css_class (GTK_WIDGET (image), "ns-glyph");
  }
}

static void
untrack (gpointer data, GObject *image)
{
  g_hash_table_remove (tracked, image);
}

void
ns_look_set_icon (GtkImage *image, const char *icon_name)
{
  if (!tracked)
    tracked = g_hash_table_new (g_direct_hash, g_direct_equal);
  if (!g_hash_table_contains (tracked, image)) {
    g_hash_table_add (tracked, image);
    g_object_weak_ref (G_OBJECT (image), untrack, NULL);
  }
  g_object_set_data_full (G_OBJECT (image), "ns-look-icon", g_strdup (icon_name), g_free);
  apply_icon (image);
}

/* ---- following the settings ---- */

static void
look_changed (GSettings *settings, const char *key, gpointer data)
{
  const Look *want = look_wanted ();
  if (want == look)
    return;
  look = want;
  load_css ();
  mark_window ();
  if (tracked) {
    GHashTableIter it;
    gpointer image;
    g_hash_table_iter_init (&it, tracked);
    while (g_hash_table_iter_next (&it, &image, NULL))
      apply_icon (image);
  }
}

void
ns_look_init (GtkWidget *win)
{
  window = win;
  theme = ns_settings ("de.yesman.GnomePhone.Theme");
  iface = ns_settings ("org.gnome.desktop.interface");
  if (iface && !ns_has_key (iface, "color-scheme"))
    g_clear_object (&iface);
  look = look_wanted ();
  css = gtk_css_provider_new ();
  load_css ();
  gtk_style_context_add_provider_for_display (gdk_display_get_default (), GTK_STYLE_PROVIDER (css),
                                              GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  mark_window ();
  if (theme) {
    g_signal_connect (theme, "changed::name", G_CALLBACK (look_changed), NULL);
    g_signal_connect (theme, "changed::day-look", G_CALLBACK (look_changed), NULL);
    g_signal_connect (theme, "changed::night-look", G_CALLBACK (look_changed), NULL);
  }
  if (iface)
    g_signal_connect (iface, "changed::color-scheme", G_CALLBACK (look_changed), NULL);
}
