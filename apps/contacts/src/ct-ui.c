/* Shared pieces of the Contacts screens (ct-ui.h). */
#include "ct-ui.h"
#include "ct-mnv-core.h"

#include <glib/gi18n.h>
#include <math.h>
#include <neo.h>

GdkRGBA
ct_rgba (guint32 argb)
{
  return (GdkRGBA) { ((argb >> 16) & 0xff) / 255.0f, ((argb >> 8) & 0xff) / 255.0f, (argb & 0xff) / 255.0f,
                     ((argb >> 24) & 0xff) / 255.0f };
}

GtkWidget *
ct_icon_button (const char *icon_name, const char *name, double width, double height, guint32 ripple)
{
  GtkWidget *image = gtk_image_new_from_icon_name (icon_name);
  GtkWidget *button = neo_pressable_new (image);
  GdkRGBA color = ct_rgba (ripple);

  gtk_image_set_pixel_size (GTK_IMAGE (image), 24);
  gtk_widget_set_size_request (button, (int) width, (int) height);
  gtk_widget_add_css_class (button, "ct-icon-button");
  /* unbounded: a circle of half the button's diagonal around its centre (RippleDrawable's MASK_UNKNOWN radius) */
  neo_pressable_set_ripple (NEO_PRESSABLE (button), NEO_RIPPLE_CIRCLE, hypot (width, height) / 2);
  neo_pressable_set_ripple_color (NEO_PRESSABLE (button), &color);
  gtk_accessible_update_property (GTK_ACCESSIBLE (button), GTK_ACCESSIBLE_PROPERTY_LABEL, name, -1);
  return button;
}

#define CT_TYPE_TINTED_ICON (ct_tinted_icon_get_type ())
G_DECLARE_FINAL_TYPE (CtTintedIcon, ct_tinted_icon, CT, TINTED_ICON, GtkWidget)

struct _CtTintedIcon {
  GtkWidget parent;
  char *icon_name;
  double size;
  GdkRGBA color;
};

G_DEFINE_FINAL_TYPE (CtTintedIcon, ct_tinted_icon, GTK_TYPE_WIDGET)

static void
ct_tinted_icon_measure (GtkWidget *widget, GtkOrientation orientation, int for_size, int *minimum, int *natural,
                        int *minimum_baseline, int *natural_baseline)
{
  *minimum = *natural = (int) CT_TINTED_ICON (widget)->size;
}

static void
ct_tinted_icon_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  CtTintedIcon *self = CT_TINTED_ICON (widget);

  if (self->icon_name)
    neo_snapshot_icon (snapshot, widget, self->icon_name, 0, 0, self->size, &self->color);
}

static void
ct_tinted_icon_finalize (GObject *object)
{
  g_free (CT_TINTED_ICON (object)->icon_name);
  G_OBJECT_CLASS (ct_tinted_icon_parent_class)->finalize (object);
}

static void
ct_tinted_icon_class_init (CtTintedIconClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = ct_tinted_icon_finalize;
  GTK_WIDGET_CLASS (klass)->measure = ct_tinted_icon_measure;
  GTK_WIDGET_CLASS (klass)->snapshot = ct_tinted_icon_snapshot;
  gtk_widget_class_set_css_name (GTK_WIDGET_CLASS (klass), "ct-tinted-icon");
}

static void
ct_tinted_icon_init (CtTintedIcon *self)
{
  gtk_widget_set_can_target (GTK_WIDGET (self), FALSE);
}

GtkWidget *
ct_tinted_icon_new (const char *icon_name, double size, guint32 argb)
{
  CtTintedIcon *self = g_object_new (CT_TYPE_TINTED_ICON, NULL);

  self->size = size;
  ct_tinted_icon_set (GTK_WIDGET (self), icon_name, argb);
  return GTK_WIDGET (self);
}

void
ct_tinted_icon_set (GtkWidget *icon, const char *icon_name, guint32 argb)
{
  CtTintedIcon *self = CT_TINTED_ICON (icon);

  g_free (self->icon_name);
  self->icon_name = g_strdup (icon_name);
  self->color = ct_rgba (argb);
  gtk_widget_queue_draw (icon);
}

GtkWidget *
ct_label (const char *text, const char *css_class, double size)
{
  GtkWidget *label = gtk_label_new (text);

  gtk_widget_add_css_class (label, css_class);
  gtk_label_set_xalign (GTK_LABEL (label), 0);
  neo_label_set_font_padding (label, size);
  return label;
}

GtkWidget *
ct_text_button (const char *label, const char *css_class)
{
  g_autofree char *caps = g_utf8_strup (label, -1);
  GtkWidget *text = gtk_label_new (caps);
  GtkWidget *button;
  GdkRGBA ripple = ct_rgba (CT_RIPPLE_APPCOMPAT);

  neo_label_set_font_padding (text, 14);
  gtk_widget_set_valign (text, GTK_ALIGN_CENTER);
  button = neo_pressable_new (text);
  gtk_widget_add_css_class (button, "ct-text-button");
  if (css_class)
    gtk_widget_add_css_class (button, css_class);
  neo_pressable_set_ripple (NEO_PRESSABLE (button), NEO_RIPPLE_BOUNDED, 2);
  neo_pressable_set_ripple_color (NEO_PRESSABLE (button), &ripple);
  gtk_accessible_update_property (GTK_ACCESSIBLE (button), GTK_ACCESSIBLE_PROPERTY_LABEL, label, -1);
  return button;
}

/* abc_btn_check_to_on_mtrl_015 / _000 at rest: in a 32 dp box, an 18 dp square from (7, 7), 2 dp outline and
 * corners; checked, the square is filled and the mark (a path from the vector) cut out of it */
void
ct_snapshot_checkbox (GtkSnapshot *snapshot, double x, double y, gboolean checked, guint32 on, guint32 off)
{
  GskRoundedRect outer;
  graphene_rect_t box = GRAPHENE_RECT_INIT ((float) x + 7, (float) y + 7, 18, 18);
  GdkRGBA color = ct_rgba (checked ? on : off);

  gsk_rounded_rect_init_from_rect (&outer, &box, 2);
  if (!checked)
    {
      float widths[4] = { 2, 2, 2, 2 };
      GdkRGBA colors[4] = { color, color, color, color };

      gtk_snapshot_append_border (snapshot, &outer, widths, colors);
      return;
    }
  gtk_snapshot_push_rounded_clip (snapshot, &outer);
  gtk_snapshot_append_color (snapshot, &color, &box);
  gtk_snapshot_pop (snapshot);
  {
    /* the check: M 9.6 14.2 L 12.4 17 L 18.4 11 in the 24 dp vector's units, drawn white (the knocked-out mark
     * shows the list's background, which is near enough to the window colour) */
    g_autoptr(GskPathBuilder) builder = gsk_path_builder_new ();
    g_autoptr(GskPath) path = NULL;
    g_autoptr(GskStroke) stroke = gsk_stroke_new (2);
    GdkRGBA mark = ct_rgba (CT_C_WINDOW_BG);
    double ox = x + 4, oy = y + 4;     /* the 24 dp vector centred in the 32 dp box */

    gsk_path_builder_move_to (builder, (float) (ox + 6.6), (float) (oy + 12.2));
    gsk_path_builder_line_to (builder, (float) (ox + 10.0), (float) (oy + 15.6));
    gsk_path_builder_line_to (builder, (float) (ox + 17.4), (float) (oy + 8.2));
    path = gsk_path_builder_to_path (builder);
    gtk_snapshot_append_stroke (snapshot, path, stroke, &mark);
  }
}

#define CT_TYPE_CHECK (ct_check_get_type ())
G_DECLARE_FINAL_TYPE (CtCheck, ct_check, CT, CHECK, GtkWidget)

struct _CtCheck {
  GtkWidget parent;
  gboolean active;
  guint32 on, off;
};

G_DEFINE_FINAL_TYPE (CtCheck, ct_check, GTK_TYPE_WIDGET)

static void
ct_check_measure (GtkWidget *widget, GtkOrientation orientation, int for_size, int *minimum, int *natural,
                  int *minimum_baseline, int *natural_baseline)
{
  *minimum = *natural = 32;
}

static void
ct_check_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  CtCheck *self = CT_CHECK (widget);

  ct_snapshot_checkbox (snapshot, 0, (gtk_widget_get_height (widget) - 32) / 2.0, self->active, self->on, self->off);
}

static void
ct_check_class_init (CtCheckClass *klass)
{
  GTK_WIDGET_CLASS (klass)->measure = ct_check_measure;
  GTK_WIDGET_CLASS (klass)->snapshot = ct_check_snapshot;
  gtk_widget_class_set_css_name (GTK_WIDGET_CLASS (klass), "ct-check");
}

static void
ct_check_init (CtCheck *self)
{
  gtk_widget_set_can_target (GTK_WIDGET (self), FALSE);
}

GtkWidget *
ct_check_new (guint32 on, guint32 off)
{
  CtCheck *self = g_object_new (CT_TYPE_CHECK, NULL);

  self->on = on;
  self->off = off;
  return GTK_WIDGET (self);
}

void
ct_check_set_active (GtkWidget *check, gboolean active)
{
  CT_CHECK (check)->active = active;
  gtk_widget_queue_draw (check);
}

gboolean
ct_check_get_active (GtkWidget *check)
{
  return CT_CHECK (check)->active;
}

/* Calls, texts, mail and maps leave the app as URIs to whatever handles their scheme (x-scheme-handler/tel and
 * the rest, as GNOME Contacts does): on this phone the Dialer and Messaging take tel: and sms: through their
 * GApplication "open", on another Mobian phone GNOME Calls and Chatty do. The app never talks to a modem itself. */
gboolean
ct_can_open_scheme (const char *scheme)
{
  g_autoptr(GAppInfo) app = g_app_info_get_default_for_uri_scheme (scheme);

  return app != NULL;
}

gboolean
ct_open_uri (GtkWidget *widget, const char *uri)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GdkAppLaunchContext) context = gdk_display_get_app_launch_context (gtk_widget_get_display (widget));

  if (!g_app_info_launch_default_for_uri (uri, G_APP_LAUNCH_CONTEXT (context), &error))
    {
      g_message ("contacts: nothing opens %s: %s", uri, error->message);
      return FALSE;
    }
  return TRUE;
}

void
ct_copy_text (GtkWidget *widget, const char *text)
{
  gdk_clipboard_set_text (gtk_widget_get_clipboard (widget), text);
}

CtNameOrder
ct_display_order (GSettings *settings)
{
  g_autofree char *order = g_settings_get_string (settings, "display-order");

  return g_str_equal (order, "last-name-first") ? CT_ORDER_ALTERNATIVE : CT_ORDER_PRIMARY;
}

CtNameOrder
ct_sort_order (GSettings *settings)
{
  g_autofree char *order = g_settings_get_string (settings, "sort-order");

  return g_str_equal (order, "last-name") ? CT_ORDER_ALTERNATIVE : CT_ORDER_PRIMARY;
}

gboolean
ct_book_shown (GSettings *settings, const char *book_uid)
{
  g_autofree char *filter = g_settings_get_string (settings, "contacts-to-display");
  g_auto(GStrv) hidden = NULL;

  if (!g_str_equal (filter, "custom"))
    return TRUE;
  hidden = g_settings_get_strv (settings, "hidden-books");
  return !g_strv_contains ((const char *const *) hidden, book_uid);
}

GPtrArray *
ct_shown_entries (CtStore *store, GSettings *settings)
{
  GPtrArray *entries = ct_store_dup_entries (store);

  for (guint i = entries->len; i > 0; i--)
    if (!ct_book_shown (settings, ((CtEntry *) entries->pdata[i - 1])->book_uid))
      g_ptr_array_remove_index_fast (entries, i - 1);
  return entries;
}

char *
ct_contacts_count (guint count, const char *account)
{
  /* contacts_count / contacts_count_with_account (§5.1.2) */
  if (account)
    return g_strdup_printf (g_dngettext (NULL, "%u contact · %s", "%u contacts · %s", count), count, account);
  return g_strdup_printf (g_dngettext (NULL, "%u contact", "%u contacts", count), count);
}

/* ---- the busy line ---- */

typedef struct {
  GtkWidget *spinner, *label;
  char *what, *slow;            /* "{s}" in @slow is the seconds so far */
  guint slow_s, timer;
  gint64 started;
} Busy;

static void
busy_free (gpointer data)
{
  Busy *busy = data;

  g_clear_handle_id (&busy->timer, g_source_remove);
  g_free (busy->what);
  g_free (busy->slow);
  g_free (busy);
}

static void
busy_draw (Busy *busy)
{
  guint seconds = (guint) ((g_get_monotonic_time () - busy->started) / G_USEC_PER_SEC);

  if (busy->slow && *busy->slow && seconds >= busy->slow_s)
    {
      g_autofree char *count = g_strdup_printf ("%u", seconds);
      g_autofree char *slow = NULL;
      const char *const pairs[] = { "s", count, NULL };

      slow = ct_mnv_format (busy->slow, pairs);
      gtk_label_set_text (GTK_LABEL (busy->label), slow);
    }
  else
    gtk_label_set_text (GTK_LABEL (busy->label), busy->what);
}

static gboolean
busy_tick (gpointer data)
{
  busy_draw (data);
  return G_SOURCE_CONTINUE;
}

GtkWidget *
ct_busy_new (void)
{
  Busy *busy = g_new0 (Busy, 1);
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);

  busy->spinner = gtk_spinner_new ();
  busy->label = ct_label ("", "ct-busy-text", 14);
  gtk_label_set_wrap (GTK_LABEL (busy->label), TRUE);
  gtk_label_set_xalign (GTK_LABEL (busy->label), 0);
  gtk_widget_set_hexpand (busy->label, TRUE);
  gtk_box_append (GTK_BOX (box), busy->spinner);
  gtk_box_append (GTK_BOX (box), busy->label);
  gtk_widget_add_css_class (box, "ct-busy");
  gtk_widget_set_visible (box, FALSE);
  g_object_set_data_full (G_OBJECT (box), "ct-busy", busy, busy_free);
  return box;
}

void
ct_busy_start (GtkWidget *box, const char *what, const char *slow, guint slow_s)
{
  Busy *busy = g_object_get_data (G_OBJECT (box), "ct-busy");
  gboolean same = busy->timer && !g_strcmp0 (busy->what, what);

  g_free (busy->what);
  g_free (busy->slow);
  busy->what = g_strdup (what ? what : "");
  busy->slow = g_strdup (slow);
  busy->slow_s = slow_s;
  if (!same)
    busy->started = g_get_monotonic_time ();      /* asked again with the same line: the clock keeps running */
  if (!busy->timer)
    busy->timer = g_timeout_add_seconds (1, busy_tick, busy);
  gtk_spinner_start (GTK_SPINNER (busy->spinner));
  gtk_widget_set_visible (box, TRUE);
  busy_draw (busy);
}

void
ct_busy_stop (GtkWidget *box)
{
  Busy *busy = g_object_get_data (G_OBJECT (box), "ct-busy");

  g_clear_handle_id (&busy->timer, g_source_remove);
  gtk_spinner_stop (GTK_SPINNER (busy->spinner));
  gtk_widget_set_visible (box, FALSE);
}

gboolean
ct_busy_active (GtkWidget *box)
{
  Busy *busy = g_object_get_data (G_OBJECT (box), "ct-busy");

  return busy->timer != 0;
}
