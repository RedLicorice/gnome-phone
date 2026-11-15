/* Lock screen and Keyboard: what the shell itself follows.
 *
 * The lock screen is gnome-phone's keyguard (extension/keyguard.js). It reads de.yesman.neo lock-type at every lock
 * to pick its bouncer: none (swipe), the pattern or the PIN pad. Pattern and PIN are both the user's own password,
 * checked by gp-auth through PAM; ns-pattern.c sets it. The keyguard's notification icons follow GNOME's
 * show-in-lock-screen, globally and per app (Notifications).
 *
 * The on-screen keyboard is GNOME Shell's own. On a touchscreen it opens by itself (keyboard.js: touch mode and the
 * last device a touchscreen); screen-keyboard-enabled keeps it on with a hardware keyboard or a mouse in use too.
 * Its layouts, like a hardware keyboard's, are org.gnome.desktop.input-sources, named from the XKB rules file. */
#include "ns-pages.h"
#include "ns-pattern.h"
#include "ns-ui.h"

/* ---- the lock type ---- */

static const char *const lock_labels[] = { "Swipe", "Pattern", "PIN", NULL };
static const char *const lock_types[] = { "swipe", "pattern", "pin" };

typedef struct {
  GSettings *neo;
  GtkWidget *type, *change;
} Lock;

/* the keyguard's own reading: "swipe" and "pattern" by name, anything else is the PIN pad */
static guint
lock_index (Lock *l)
{
  g_autofree char *t = g_settings_get_string (l->neo, "lock-type");
  for (guint i = 0; i < G_N_ELEMENTS (lock_types); i++)
    if (!g_strcmp0 (t, lock_types[i]))
      return i;
  return 2;
}

static void
lock_show (Lock *l)
{
  guint i = lock_index (l);
  ns_row_pick_select (l->type, i);
  gtk_widget_set_visible (l->change, i != 0);
  gtk_label_set_text (GTK_LABEL (g_object_get_data (G_OBJECT (l->change), "ns-title-label")),
                      i == 1 ? "Change pattern" : "Change PIN");
}

typedef struct {
  Lock      *l;
  guint      i;
  GtkWidget *row;
} LockSet;

/* the pattern (or PIN) dialog is done: the type changes only once the new secret is the password */
static void
secret_done (gboolean ok, gpointer data)
{
  LockSet *ls = data;
  if (ok)
    g_settings_set_string (ls->l->neo, "lock-type", lock_types[ls->i]);
  lock_show (ls->l);
  g_object_unref (ls->row);
  g_free (ls);
}

static void
secret_dialog (Lock *l, GtkWidget *row, guint i)
{
  LockSet *ls = g_new0 (LockSet, 1);
  ls->l = l;
  ls->i = i;
  ls->row = g_object_ref (row);
  ns_pattern_dialog_present (row, i == 2, secret_done, ls);
}

static void
lock_picked (GtkWidget *row, guint i, gpointer data)
{
  Lock *l = data;
  if (i == lock_index (l))
    return;
  /* a pattern or a PIN counts once it is entered, confirmed and set as the password, as on Android (owner,
   * 2026-11-12); swipe needs none. A PIN does not keep the password there is: the lock screen's PIN pad types digits
   * only, and a password with a letter in it could not be typed there to unlock. */
  if (i != 0) {
    adw_navigation_view_pop (ADW_NAVIGATION_VIEW (gtk_widget_get_ancestor (row, ADW_TYPE_NAVIGATION_VIEW)));
    secret_dialog (l, row, i);
    lock_show (l);
    return;
  }
  g_settings_set_string (l->neo, "lock-type", lock_types[i]);
}

static void
change_tapped (GtkWidget *row, gpointer data)
{
  Lock *l = data;
  secret_dialog (l, row, lock_index (l));
}

static void
lock_free (Lock *l)
{
  g_signal_handlers_disconnect_by_data (l->neo, l);
  g_object_unref (l->neo);
  g_free (l);
}

/* Not on this page, and why:
 * - "Lock automatically" / "Lock after the screen turns off" (org.gnome.desktop.screensaver lock-enabled and
 *   lock-delay): the keyguard locks the moment the screen goes off (power.js goToSleep calls keyguard.lock()). A
 *   delay would need power.js to note when the screen went off and to lock on the next wake only past the delay.
 * - A lock screen wallpaper of its own (the screensaver's picture-uri): the keyguard shows the home screen's
 *   wallpaper, as Shell 51's own lock screen does.
 * - Shuffling the PIN keys (Phosh's shuffle-keypad): the keyguard's keypad is fixed.
 * - Lock screen widgets (Phosh's plugins): the keyguard has none. */
void
ns_page_lock_screen (GtkWidget *row, gpointer data)
{
  GSettings *neo = ns_settings ("de.yesman.neo");
  if (!neo || !ns_has_key (neo, "lock-type")) {
    g_clear_object (&neo);
    return;
  }
  GtkWidget *c;
  AdwNavigationPage *page = ns_page_new ("Lock screen", &c);
  Lock *l = g_new0 (Lock, 1);
  l->neo = neo;
  g_object_set_data_full (G_OBJECT (page), "ns-lock", l, (GDestroyNotify) lock_free);

  GtkWidget *g = ns_group (c, NULL);
  l->type = ns_row_pick (g, "Screen lock type", lock_labels, lock_index (l), lock_picked, l);
  l->change = ns_row (g, NULL, NULL, "Change PIN", "It is also your password");
  ns_row_on_activate (l->change, change_tapped, l);
  g_signal_connect_swapped (neo, "changed::lock-type", G_CALLBACK (lock_show), l);
  lock_show (l);
  ns_note (c, "Swipe unlocks without a password. Pattern and PIN unlock with your password: a pattern or PIN set "
              "here becomes your password (also for sudo, SSH and the console).");

  ns_fprint_lock_group (page, c);

  g = ns_group (c, "On the lock screen");
  GSettings *n = ns_page_settings (page, "org.gnome.desktop.notifications");
  ns_row_switch (g, "Notifications", "Their apps' icons below the clock", n, "show-in-lock-screen");
  ns_push (page);
}

/* ---- keyboard layouts: org.gnome.desktop.input-sources, named from the XKB rules ---- */

/* the XKB registry every keyboard layout comes from (xkb-data); GNOME's own layout list reads it too */
#define XKB_RULES "/usr/share/X11/xkb/rules/evdev.xml"
/* where each layout's keys are, a file named as the layout */
#define XKB_SYMBOLS "/usr/share/X11/xkb/symbols"

typedef struct {
  char *id, *name;             /* "us" and "English (US)" */
} Layout;

typedef struct {
  GSettings *src;
  GPtrArray *all;              /* every Layout of the rules file, by name */
  GtkWidget *group;
} Layouts;

static void
layout_free (Layout *l)
{
  g_free (l->id);
  g_free (l->name);
  g_free (l);
}

static void
layouts_free (Layouts *l)
{
  g_ptr_array_unref (l->all);
  g_free (l);
}

/* Reading the rules file: <layoutList><layout><configItem><name>us</name><description>English (US)</description>.
 * A layout's variants sit in its <variantList>, with configItems of their own: those are skipped, so the list
 * has the layouts only (about a hundred), not their six hundred variants. */
typedef struct {
  GPtrArray *out;
  int        variants;         /* > 0 while inside a <variantList> */
  GString   *text;             /* the text of the <name> or <description> being read, or NULL */
  char      *id, *name;        /* the current layout's */
} XkbRead;

static void
xkb_start (GMarkupParseContext *ctx, const char *element, const char **names, const char **values, gpointer data,
           GError **error)
{
  XkbRead *r = data;
  if (!g_strcmp0 (element, "variantList"))
    r->variants++;
  else if (!r->variants && (!g_strcmp0 (element, "name") || !g_strcmp0 (element, "description")))
    r->text = g_string_new (NULL);
}

static void
xkb_text (GMarkupParseContext *ctx, const char *text, gsize len, gpointer data, GError **error)
{
  XkbRead *r = data;
  if (r->text)
    g_string_append_len (r->text, text, len);
}

static void
xkb_end (GMarkupParseContext *ctx, const char *element, gpointer data, GError **error)
{
  XkbRead *r = data;
  if (!g_strcmp0 (element, "variantList")) {
    r->variants--;
  } else if (r->text && !g_strcmp0 (element, "name")) {
    g_free (r->id);
    r->id = g_string_free (g_steal_pointer (&r->text), FALSE);
  } else if (r->text && !g_strcmp0 (element, "description")) {
    g_free (r->name);
    r->name = g_string_free (g_steal_pointer (&r->text), FALSE);
  } else if (!g_strcmp0 (element, "layout")) {
    /* The layout's own configItem came first, its variants after: what is held is the layout's. A layout with no
     * symbols file could not be used, and the registry lists one: "custom", for a file of the user's own. */
    g_autofree char *symbols = r->id ? g_build_filename (XKB_SYMBOLS, r->id, NULL) : NULL;
    if (r->id && r->name && g_file_test (symbols, G_FILE_TEST_EXISTS)) {
      Layout *l = g_new0 (Layout, 1);
      l->id = g_steal_pointer (&r->id);
      l->name = g_steal_pointer (&r->name);
      g_ptr_array_add (r->out, l);
    }
    g_clear_pointer (&r->id, g_free);
    g_clear_pointer (&r->name, g_free);
  }
}

static gint
layout_by_name (gconstpointer a, gconstpointer b)
{
  return g_utf8_collate ((*(Layout **) a)->name, (*(Layout **) b)->name);
}

static GPtrArray *
xkb_layouts (void)
{
  GPtrArray *out = g_ptr_array_new_with_free_func ((GDestroyNotify) layout_free);
  g_autofree char *xml = NULL;
  gsize len;
  if (!g_file_get_contents (XKB_RULES, &xml, &len, NULL))
    return out;
  static const GMarkupParser parser = { xkb_start, xkb_end, xkb_text, NULL, NULL };
  XkbRead r = { .out = out };
  GMarkupParseContext *ctx = g_markup_parse_context_new (&parser, 0, &r, NULL);
  if (!g_markup_parse_context_parse (ctx, xml, len, NULL))
    g_warning ("%s: not read whole", XKB_RULES);
  g_markup_parse_context_free (ctx);
  if (r.text)
    g_string_free (r.text, TRUE);
  g_free (r.id);
  g_free (r.name);
  g_ptr_array_sort (out, layout_by_name);
  return out;
}

/* a source's name: the layout's description, or its id (a variant, "de+nodeadkeys", or an input method) */
static const char *
layout_name (Layouts *l, const char *type, const char *id)
{
  if (!g_strcmp0 (type, "xkb"))
    for (guint i = 0; i < l->all->len; i++)
      if (!g_strcmp0 (((Layout *) l->all->pdata[i])->id, id))
        return ((Layout *) l->all->pdata[i])->name;
  return id;
}

static void layouts_fill (AdwNavigationPage *page);

static void
layout_remove (GtkButton *btn, AdwNavigationPage *page)
{
  Layouts *l = g_object_get_data (G_OBJECT (page), "ns-layouts");
  guint drop = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (btn), "ns-index"));
  g_autoptr (GVariant) v = g_settings_get_value (l->src, "sources");
  GVariantBuilder b;
  g_variant_builder_init (&b, G_VARIANT_TYPE ("a(ss)"));
  for (guint i = 0; i < g_variant_n_children (v); i++)
    if (i != drop)
      g_variant_builder_add_value (&b, g_variant_get_child_value (v, i));
  g_settings_set_value (l->src, "sources", g_variant_builder_end (&b));
}

/* a layout picked on the "Add a layout" page: appended, so the first one stays the one used first */
static void
layout_added (GtkWidget *row, gpointer data)
{
  AdwNavigationPage *page = g_object_get_data (G_OBJECT (row), "ns-page");
  Layouts *l = g_object_get_data (G_OBJECT (page), "ns-layouts");
  Layout *add = l->all->pdata[GPOINTER_TO_UINT (data)];
  g_autoptr (GVariant) v = g_settings_get_value (l->src, "sources");
  GVariantBuilder b;
  g_variant_builder_init (&b, G_VARIANT_TYPE ("a(ss)"));
  for (guint i = 0; i < g_variant_n_children (v); i++)
    g_variant_builder_add_value (&b, g_variant_get_child_value (v, i));
  g_variant_builder_add (&b, "(ss)", "xkb", add->id);
  g_settings_set_value (l->src, "sources", g_variant_builder_end (&b));
  adw_navigation_view_pop (ADW_NAVIGATION_VIEW (gtk_widget_get_ancestor (row, ADW_TYPE_NAVIGATION_VIEW)));
}

static gboolean
has_source (GVariant *sources, const char *id)
{
  for (guint k = 0; k < g_variant_n_children (sources); k++) {
    const char *t, *have;
    g_variant_get_child (sources, k, "(&s&s)", &t, &have);
    if (!g_strcmp0 (t, "xkb") && !g_strcmp0 (have, id))
      return TRUE;
  }
  return FALSE;
}

static void
layout_add (GtkWidget *row, gpointer page)
{
  Layouts *l = g_object_get_data (G_OBJECT (page), "ns-layouts");
  GtkWidget *c;
  AdwNavigationPage *add = ns_page_new ("Add a layout", &c);
  GtkWidget *g = ns_group (c, NULL);
  g_autoptr (GVariant) v = g_settings_get_value (l->src, "sources");
  for (guint i = 0; i < l->all->len; i++) {
    Layout *one = l->all->pdata[i];
    if (has_source (v, one->id))
      continue;
    GtkWidget *r = ns_row (g, NULL, NULL, one->name, NULL);
    g_object_set_data (G_OBJECT (r), "ns-page", page);
    ns_row_on_activate (r, layout_added, GUINT_TO_POINTER (i));
  }
  ns_push (add);
}

static void
layouts_fill (AdwNavigationPage *page)
{
  Layouts *l = g_object_get_data (G_OBJECT (page), "ns-layouts");
  ns_group_clear (l->group);
  g_autoptr (GVariant) v = g_settings_get_value (l->src, "sources");
  guint n = g_variant_n_children (v);
  for (guint i = 0; i < n; i++) {
    const char *t, *id;
    g_variant_get_child (v, i, "(&s&s)", &t, &id);
    GtkWidget *r = ns_row (l->group, NULL, NULL, layout_name (l, t, id), i == 0 ? "Used first" : NULL);
    if (n > 1) {
      GtkWidget *x = gtk_button_new_from_icon_name ("user-trash-symbolic");
      gtk_widget_add_css_class (x, "flat");
      gtk_widget_add_css_class (x, "circular");
      gtk_widget_set_tooltip_text (x, "Remove");
      g_object_set_data (G_OBJECT (x), "ns-index", GUINT_TO_POINTER (i));
      g_signal_connect (x, "clicked", G_CALLBACK (layout_remove), page);
      ns_row_suffix (r, x);
    }
  }
  if (l->all->len)
    ns_row_button (l->group, "Add a layout", FALSE, layout_add, page);
}

static void
layouts_rows (AdwNavigationPage *page, GtkWidget *c)
{
  GSettings *src = ns_page_settings (page, "org.gnome.desktop.input-sources");
  if (!src)
    return;
  Layouts *l = g_new0 (Layouts, 1);
  l->src = src;
  l->all = xkb_layouts ();
  g_object_set_data_full (G_OBJECT (page), "ns-layouts", l, (GDestroyNotify) layouts_free);
  l->group = ns_group (c, "Layouts");
  ns_later_init (page, layouts_fill);
  g_signal_connect_object (src, "changed::sources", G_CALLBACK (ns_later), page, G_CONNECT_SWAPPED);
  layouts_fill (page);
}

/* Not on this page, and why: which keyboard (Stevia or Squeekboard), their word suggestions and sizes, and how
 * long a press on the home bar opens it (Phosh's osk-unfold-delay). GNOME Shell's keyboard replaced them all, and
 * it has no settings of its own besides the switch and the layouts. */
void
ns_page_keyboard (GtkWidget *row, gpointer data)
{
  GtkWidget *c;
  AdwNavigationPage *page = ns_page_new ("Keyboard", &c);
  GSettings *a11y = ns_page_settings (page, "org.gnome.desktop.a11y.applications");
  if (ns_has_key (a11y, "screen-keyboard-enabled")) {
    GtkWidget *g = ns_group (c, NULL);
    ns_row_switch (g, "Show on-screen keyboard", "Also while a hardware keyboard or a mouse is used", a11y,
                   "screen-keyboard-enabled");
    ns_note (c, "Typing on the touchscreen opens the on-screen keyboard either way.");
  }
  GtkWidget *g = ns_group (c, NULL);
  ns_row_on_activate (ns_row (g, NULL, NULL, "Touch keyboard", "Size • Look • Typing • Gestures"),
                      ns_page_touch_keyboard, NULL);
  layouts_rows (page, c);
  ns_push (page);
}

/* an integer key over a wide range as a slider that shows its value */
static void
int_slider (GtkWidget *g, const char *title, GSettings *s, const char *key, int lo, int hi, int step)
{
  GtkWidget *scale;
  ns_row_scale (g, title, NULL, lo, hi, step, &scale);
  gtk_scale_set_draw_value (GTK_SCALE (scale), TRUE);
  gtk_scale_set_digits (GTK_SCALE (scale), 0);
  gtk_scale_set_value_pos (GTK_SCALE (scale), GTK_POS_RIGHT);
  g_settings_bind (s, key, gtk_range_get_adjustment (GTK_RANGE (scale)), "value", G_SETTINGS_BIND_DEFAULT);
}

/* The touch keyboard of the gnome-phone extension: its schema comes with the extension package. */
void
ns_page_touch_keyboard (GtkWidget *row, gpointer data)
{
  static const char *const modes[] = { "compact", "full", NULL };
  static const char *const mode_labels[] = { "Compact", "Full (Hacker's Keyboard)", NULL };
  static const char *const languages[] = { "it,en", "en,it", "it", "en", NULL };
  static const char *const language_labels[] = { "Italiano + English", "English + Italiano", "Italiano", "English",
                                                  NULL };
  static const char *const styles[] = { "dark", "light", "black", "material", NULL };
  static const char *const style_labels[] = { "Dark", "Light", "Black", "Material", NULL };
  static const char *const swipes[] = { "none", "close", "extension", "settings", "height-up", "height-down",
                                        "lang-prev", "lang-next", "emoji", NULL };
  static const char *const swipe_labels[] = { "Nothing", "Close keyboard", "Function key row", "Settings", "Taller",
                                              "Shorter", "Previous language", "Next language", "Emoji", NULL };
  GtkWidget *c;
  AdwNavigationPage *page = ns_page_new ("Touch keyboard", &c);
  GSettings *s = ns_page_settings (page, "de.yesman.GnomePhone.Keyboard");
  if (!s) {
    ns_note (c, "The touch keyboard is not installed");
    ns_push (page);
    return;
  }
  GtkWidget *g = ns_group (c, NULL);
  ns_row_switch (g, "Use the touch keyboard", NULL, s, "enabled");
  g = ns_group (c, "Layout");
  ns_row_choice (g, "Layout", s, "mode", modes, mode_labels);
  ns_row_switch (g, "Bar over the keys", "Suggestions • Esc, Tab, Ctrl, Alt • Arrows", s, "bar");
  int_slider (g, "Bar height (%)", s, "bar-scale", 50, 120, 5);
  g = ns_group (c, "Size");
  int_slider (g, "Height, portrait (%)", s, "height-portrait", 15, 75, 1);
  int_slider (g, "Height, landscape (%)", s, "height-landscape", 15, 75, 1);
  int_slider (g, "Number row height, full layout (%)", s, "top-row-scale", 50, 100, 5);
  ns_row_switch (g, "Function key row", NULL, s, "extension-row");
  int_slider (g, "Bottom padding (px)", s, "bottom-padding", 0, 48, 1);
  g = ns_group (c, "Look");
  ns_row_choice (g, "Style", s, "style", styles, style_labels);
  ns_row_spin (g, "Key gap (px)", s, "key-gap", 0, 12);
  ns_row_spin (g, "Key radius (px)", s, "key-radius", 0, 24);
  ns_row_switch (g, "Raised keys", NULL, s, "key-borders");
  int_slider (g, "Label size (%)", s, "label-scale", 50, 200, 5);
  g = ns_group (c, "Typing");
  ns_row_switch (g, "Key preview", NULL, s, "key-preview");
  int_slider (g, "Long press (ms)", s, "long-press-ms", 100, 2000, 50);
  int_slider (g, "Repeat delay (ms)", s, "repeat-delay-ms", 100, 1000, 50);
  int_slider (g, "Repeat interval (ms)", s, "repeat-interval-ms", 20, 200, 10);
  ns_row_switch (g, "Suggestions", "In the bar, while typing and after a space", s, "suggestions");
  ns_row_choice (g, "Languages", s, "languages", languages, language_labels);
  ns_row_switch (g, "Learn from what I type", "A long press on a suggestion forgets it", s, "learn");
  ns_row_switch (g, "Vibrate on keypress", NULL, s, "haptics");
  int_slider (g, "Vibration (ms)", s, "haptic-ms", 5, 100, 1);
  ns_row_spin (g, "Touch offset (px)", s, "touch-offset", 0, 20);
  ns_row_spin (g, "Hysteresis (px)", s, "hysteresis", 0, 30);
  g = ns_group (c, "Gestures");
  ns_row_choice (g, "Swipe up", s, "swipe-up", swipes, swipe_labels);
  ns_row_choice (g, "Swipe down", s, "swipe-down", swipes, swipe_labels);
  ns_row_choice (g, "Swipe left", s, "swipe-left", swipes, swipe_labels);
  ns_row_choice (g, "Swipe right", s, "swipe-right", swipes, swipe_labels);
  ns_row_switch (g, "Slide on space moves the cursor", NULL, s, "space-cursor");
  ns_push (page);
}
