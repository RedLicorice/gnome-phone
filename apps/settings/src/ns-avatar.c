/* Minerva > Avatar: the owner's avatar settings, built entirely from the one schema the kiosk serves
 * (org.minerva.Kiosk1 SettingsDescribe: Minerva's kiosk/data/avatar-settings.json with each setting's default and
 * value). Each group of the schema is an accordion (AdwExpanderRow) with one row per setting: a slider for a number,
 * a switch for a flag, a drop-down for a choice; a "restore defaults" row per group and a global one. A change goes to
 * the kiosk at once (SettingsSet), which checks it against the same schema and shows it on the face live; the page
 * keeps no list of settings of its own (docs/avatar-settings.md in Minerva's repository). */
#include <adwaita.h>
#include <json-glib/json-glib.h>

#include "ns-pages.h"
#include "ns-ui.h"

/* the kiosk's D-Bus API (Minerva's kiosk/minerva_kiosk/dbusapi.py): the protocol's names */
#define KIOSK_NAME  "org.minerva.Kiosk1"
#define KIOSK_PATH  "/org/minerva/Kiosk1"
#define KIOSK_IFACE "org.minerva.Kiosk1"
/* a slider's changes are sent once the finger rests this long: the kiosk rewrites its file on each set */
#define SLIDER_SETTLE_MS 250

typedef struct {
  AdwNavigationPage *page;
  GtkWidget         *content;
  GtkWidget         *card;      /* the groups' accordions */
  GDBusConnection   *bus;
  JsonObject        *texts;     /* the schema's own texts (reset rows, the error) */
  const char        *lang;      /* "it" or "en": the labels' language */
  GHashTable        *controls;  /* key -> its control widget (to show a value again) */
  gboolean           syncing;   /* the page is showing values: no set */
} Av;

static void av_load (Av *a);

static const char *
text_of (JsonObject *labels, const char *lang)
{
  if (labels == NULL)
    return "";
  if (json_object_has_member (labels, lang))
    return json_object_get_string_member (labels, lang);
  return json_object_has_member (labels, "en") ? json_object_get_string_member (labels, "en") : "";
}

static const char *
av_text (Av *a, const char *id)
{
  return text_of (a->texts && json_object_has_member (a->texts, id) ? json_object_get_object_member (a->texts, id) : NULL,
                  a->lang);
}

static void
av_free (Av *a)
{
  g_clear_object (&a->bus);
  g_clear_pointer (&a->texts, json_object_unref);
  g_clear_pointer (&a->controls, g_hash_table_unref);
  g_free (a);
}

/* ---- talking to the kiosk ---- */

typedef void (*AvReply) (Av *a, JsonNode *reply, const char *err);

typedef struct {
  Av      *a;
  AvReply  done;
} AvCall;

static void
call_done (GObject *src, GAsyncResult *res, gpointer data)
{
  AvCall *c = data;
  g_autoptr (GError) err = NULL;
  g_autoptr (GVariant) out = g_dbus_connection_call_finish (G_DBUS_CONNECTION (src), res, &err);
  g_autoptr (JsonParser) parser = json_parser_new ();
  const char *text = NULL;

  if (out != NULL) {
    g_variant_get (out, "(&s)", &text);
    if (json_parser_load_from_data (parser, text, -1, NULL))
      c->done (c->a, json_parser_get_root (parser), NULL);
    else
      c->done (c->a, NULL, av_text (c->a, "unavailable"));
  } else if (!g_error_matches (err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
    g_autofree char *msg = g_dbus_error_get_remote_error (err) ? g_strdup (err->message) : NULL;
    if (msg)
      g_dbus_error_strip_remote_error (err);
    c->done (c->a, NULL, msg ? err->message : av_text (c->a, "unavailable"));
  }
  g_object_unref (c->a->page);
  g_free (c);
}

static void
av_call (Av *a, const char *method, GVariant *args, AvReply done)
{
  AvCall *c = g_new0 (AvCall, 1);

  c->a = a;
  c->done = done;
  g_object_ref (a->page);   /* the page outlives the call */
  g_dbus_connection_call (a->bus, KIOSK_NAME, KIOSK_PATH, KIOSK_IFACE, method, args, G_VARIANT_TYPE ("(s)"),
                          G_DBUS_CALL_FLAGS_NO_AUTO_START, -1, NULL, call_done, c);
}

static void
say (Av *a, const char *err)
{
  if (err && *err)
    ns_note (a->content, err);
}

/* ---- showing a value ---- */

static void
show_value (Av *a, const char *key, JsonNode *value)
{
  GtkWidget *w = g_hash_table_lookup (a->controls, key);

  if (w == NULL || value == NULL || JSON_NODE_HOLDS_NULL (value))
    return;
  a->syncing = TRUE;
  if (GTK_IS_SWITCH (w)) {
    gtk_switch_set_active (GTK_SWITCH (w), json_node_get_boolean (value));
  } else if (GTK_IS_RANGE (w)) {
    gtk_range_set_value (GTK_RANGE (w), json_node_get_double (value));
  } else if (GTK_IS_DROP_DOWN (w)) {
    GStrv ids = g_object_get_data (G_OBJECT (w), "av-ids");
    for (guint i = 0; ids && ids[i]; i++)
      if (!g_strcmp0 (ids[i], json_node_get_string (value)))
        gtk_drop_down_set_selected (GTK_DROP_DOWN (w), i);
  }
  a->syncing = FALSE;
}

/* ---- changing a value ---- */

static void
set_done (Av *a, JsonNode *reply, const char *err)
{
  if (err) {
    say (a, err);
    av_load (a);   /* what the kiosk really holds */
    return;
  }
  JsonObject *o = json_node_get_object (reply);
  show_value (a, json_object_get_string_member (o, "key"), json_object_get_member (o, "value"));
}

static void
av_set (Av *a, const char *key, const char *json_value)
{
  av_call (a, "SettingsSet", g_variant_new ("(ss)", key, json_value), set_done);
}

static void
switch_changed (GtkSwitch *sw, GParamSpec *ps, Av *a)
{
  if (!a->syncing)
    av_set (a, g_object_get_data (G_OBJECT (sw), "av-key"), gtk_switch_get_active (sw) ? "true" : "false");
}

static void
drop_changed (GtkDropDown *dd, GParamSpec *ps, Av *a)
{
  GStrv ids = g_object_get_data (G_OBJECT (dd), "av-ids");
  guint i = gtk_drop_down_get_selected (dd);

  if (a->syncing || ids == NULL || i >= g_strv_length (ids))
    return;
  g_autofree char *quoted = g_strdup_printf ("\"%s\"", ids[i]);
  av_set (a, g_object_get_data (G_OBJECT (dd), "av-key"), quoted);
}

static gboolean
slider_settled (gpointer data)
{
  GtkRange *r = data;
  Av *a = g_object_get_data (G_OBJECT (r), "av");
  char buf[G_ASCII_DTOSTR_BUF_SIZE];

  g_object_set_data (G_OBJECT (r), "av-timer", NULL);
  av_set (a, g_object_get_data (G_OBJECT (r), "av-key"), g_ascii_dtostr (buf, sizeof buf, gtk_range_get_value (r)));
  return G_SOURCE_REMOVE;
}

static void
slider_changed (GtkRange *r, Av *a)
{
  GtkWidget *label = g_object_get_data (G_OBJECT (r), "av-label");
  const char *unit = g_object_get_data (G_OBJECT (r), "av-unit");
  int digits = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (r), "av-digits"));
  g_autofree char *shown = g_strdup_printf ("%.*f%s%s", digits, gtk_range_get_value (r), *unit ? " " : "", unit);
  guint timer = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (r), "av-timer"));

  gtk_label_set_text (GTK_LABEL (label), shown);
  if (a->syncing)
    return;
  if (timer)
    g_source_remove (timer);
  timer = g_timeout_add (SLIDER_SETTLE_MS, slider_settled, r);
  g_object_set_data (G_OBJECT (r), "av-timer", GUINT_TO_POINTER (timer));
}

static void
slider_gone (gpointer data, GObject *where)
{
  guint timer = GPOINTER_TO_UINT (g_object_get_data (where, "av-timer"));
  if (timer)
    g_source_remove (timer);
}

/* ---- one row per setting ---- */

static int
digits_of (double step)
{
  int d = 0;
  while (step > 0 && step < 1 && d < 3) {
    step *= 10;
    d++;
  }
  return d;
}

static GtkWidget *
setting_row (Av *a, JsonObject *s)
{
  const char *key = json_object_get_string_member (s, "key");
  const char *type = json_object_get_string_member (s, "type");
  JsonObject *help = json_object_has_member (s, "help") ? json_object_get_object_member (s, "help") : NULL;
  GtkWidget *row = adw_action_row_new ();
  GtkWidget *control = NULL;
  g_autoptr (GString) sub = g_string_new (text_of (help, a->lang));

  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), text_of (json_object_get_object_member (s, "label"), a->lang));
  if (!g_strcmp0 (json_object_get_string_member_with_default (s, "apply", ""), "new"))
    g_string_append_printf (sub, "%s%s", sub->len ? " • " : "", av_text (a, "applies_new"));
  adw_action_row_set_subtitle (ADW_ACTION_ROW (row), sub->str);

  if (!g_strcmp0 (type, "bool")) {
    control = gtk_switch_new ();
    gtk_widget_set_valign (control, GTK_ALIGN_CENTER);
    g_signal_connect (control, "notify::active", G_CALLBACK (switch_changed), a);
    adw_action_row_add_suffix (ADW_ACTION_ROW (row), control);
    adw_action_row_set_activatable_widget (ADW_ACTION_ROW (row), control);
  } else if (!g_strcmp0 (type, "choice")) {
    JsonArray *choices = json_object_get_array_member (s, "choices");
    JsonObject *labels = json_object_has_member (s, "choice_labels") ? json_object_get_object_member (s, "choice_labels") : NULL;
    guint n = choices ? json_array_get_length (choices) : 0;
    GStrv ids = g_new0 (char *, n + 1);
    g_autoptr (GtkStringList) shown = gtk_string_list_new (NULL);
    for (guint i = 0; i < n; i++) {
      const char *id = json_array_get_string_element (choices, i);
      ids[i] = g_strdup (id);
      gtk_string_list_append (shown, labels && json_object_has_member (labels, id)
                                     ? text_of (json_object_get_object_member (labels, id), a->lang) : id);
    }
    control = gtk_drop_down_new (G_LIST_MODEL (g_object_ref (shown)), NULL);
    g_object_set_data_full (G_OBJECT (control), "av-ids", ids, (GDestroyNotify) g_strfreev);
    gtk_widget_set_valign (control, GTK_ALIGN_CENTER);
    g_signal_connect (control, "notify::selected", G_CALLBACK (drop_changed), a);
    adw_action_row_add_suffix (ADW_ACTION_ROW (row), control);
  } else {   /* int, float: a slider with its value */
    double lo = json_object_get_double_member (s, "min"), hi = json_object_get_double_member (s, "max");
    double step = json_object_get_double_member_with_default (s, "step", !g_strcmp0 (type, "int") ? 1 : 0.01);
    GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *label = gtk_label_new ("");
    control = gtk_scale_new_with_range (GTK_ORIENTATION_HORIZONTAL, lo, hi, step);
    gtk_scale_set_draw_value (GTK_SCALE (control), FALSE);
    gtk_widget_set_size_request (control, 150, -1);
    gtk_label_set_xalign (GTK_LABEL (label), 1);
    gtk_widget_add_css_class (label, "dim-label");
    gtk_box_append (GTK_BOX (box), label);
    gtk_box_append (GTK_BOX (box), control);
    gtk_widget_set_valign (box, GTK_ALIGN_CENTER);
    g_object_set_data (G_OBJECT (control), "av", a);
    g_object_set_data (G_OBJECT (control), "av-label", label);
    g_object_set_data_full (G_OBJECT (control), "av-unit", g_strdup (json_object_get_string_member_with_default (s, "unit", "")), g_free);
    g_object_set_data (G_OBJECT (control), "av-digits", GINT_TO_POINTER (digits_of (step)));
    g_signal_connect (control, "value-changed", G_CALLBACK (slider_changed), a);
    g_object_weak_ref (G_OBJECT (control), slider_gone, NULL);
    adw_action_row_add_suffix (ADW_ACTION_ROW (row), box);
  }
  g_object_set_data_full (G_OBJECT (control), "av-key", g_strdup (key), g_free);
  g_hash_table_insert (a->controls, g_strdup (key), control);
  show_value (a, key, json_object_get_member (s, "value"));
  if (GTK_IS_RANGE (control)) {
    a->syncing = TRUE;   /* the value label only: no set */
    slider_changed (GTK_RANGE (control), a);
    a->syncing = FALSE;
  }
  return row;
}

/* ---- resets ---- */

static void
reset_done (Av *a, JsonNode *reply, const char *err)
{
  say (a, err);
  av_load (a);
}

static void
reset_group (GtkWidget *row, gpointer data)
{
  Av *a = g_object_get_data (G_OBJECT (row), "av");
  av_call (a, "SettingsReset", g_variant_new ("(s)", (const char *) data), reset_done);
}

static void
reset_all_confirmed (gboolean yes, gpointer data)
{
  Av *a = data;
  if (!yes)
    return;
  av_call (a, "SettingsReset", g_variant_new ("(s)", ""), reset_done);
}

static void
reset_all (GtkWidget *row, gpointer data)
{
  Av *a = data;
  ns_confirm (GTK_WIDGET (a->page), av_text (a, "reset_all"), av_text (a, "reset_all_body"), av_text (a, "reset_all"),
              TRUE, reset_all_confirmed, a);
}

/* ---- the page ---- */

static void
described (Av *a, JsonNode *reply, const char *err)
{
  if (err) {
    say (a, err);
    return;
  }
  JsonObject *root = json_node_get_object (reply);
  JsonArray *settings = json_object_get_array_member (root, "settings");

  if (a->card != NULL) {   /* a second look (after a reset): only the values */
    for (guint i = 0; i < json_array_get_length (settings); i++) {
      JsonObject *s = json_array_get_object_element (settings, i);
      show_value (a, json_object_get_string_member (s, "key"), json_object_get_member (s, "value"));
    }
    return;
  }
  JsonArray *groups = json_object_get_array_member (root, "groups");
  a->card = ns_group (a->content, NULL);
  for (guint g = 0; g < json_array_get_length (groups); g++) {
    JsonObject *grp = json_array_get_object_element (groups, g);
    const char *id = json_object_get_string_member (grp, "id");
    GtkWidget *exp = adw_expander_row_new ();
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (exp), text_of (json_object_get_object_member (grp, "label"), a->lang));
    for (guint i = 0; i < json_array_get_length (settings); i++) {
      JsonObject *s = json_array_get_object_element (settings, i);
      if (!g_strcmp0 (json_object_get_string_member (s, "group"), id))
        adw_expander_row_add_row (ADW_EXPANDER_ROW (exp), setting_row (a, s));
    }
    GtkWidget *reset = adw_action_row_new ();
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (reset), av_text (a, "reset_group"));
    gtk_list_box_row_set_activatable (GTK_LIST_BOX_ROW (reset), TRUE);
    gtk_widget_add_css_class (reset, "accent");
    g_object_set_data (G_OBJECT (reset), "av", a);
    g_signal_connect_data (reset, "activated", G_CALLBACK (reset_group), g_strdup (id), (GClosureNotify) g_free, 0);
    adw_expander_row_add_row (ADW_EXPANDER_ROW (exp), reset);
    gtk_list_box_append (GTK_LIST_BOX (a->card), exp);
  }
  ns_row_button (ns_group (a->content, NULL), av_text (a, "reset_all"), TRUE, reset_all, a);
}

static void
described_texts (Av *a, JsonNode *reply, const char *err)
{
  if (reply != NULL) {
    JsonObject *root = json_node_get_object (reply);
    if (json_object_has_member (root, "texts")) {
      g_clear_pointer (&a->texts, json_object_unref);
      a->texts = json_object_ref (json_object_get_object_member (root, "texts"));
    }
  }
  described (a, reply, err);
}

static void
av_load (Av *a)
{
  av_call (a, "SettingsDescribe", NULL, described_texts);
}

void
ns_page_minerva_avatar (GtkWidget *row, gpointer data)
{
  Av *a = g_new0 (Av, 1);
  const char *const *langs = g_get_language_names ();

  a->lang = langs[0] && g_str_has_prefix (langs[0], "it") ? "it" : "en";
  a->controls = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  a->page = ns_page_new ("Avatar", &a->content);
  g_object_set_data_full (G_OBJECT (a->page), "ns-avatar", a, (GDestroyNotify) av_free);
  a->bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, NULL);
  if (a->bus != NULL)
    av_load (a);
  ns_push (a->page);
}
