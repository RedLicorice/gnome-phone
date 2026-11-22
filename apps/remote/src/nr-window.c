/* Neo Remote's window: see nr-window.h.
 *
 * Pages: home (the remotes and the macros), remote (its buttons as a grid; a tap sends, a long press edits; a macro is
 * recorded by tapping buttons), brands and scan (the universal-remote scan: the brand's Power codes one at a time,
 * "reacted" keeps that remote), search (a model of the code database). Every action is a call of org.minerva.Ir1. */
#include "nr-window.h"
#include "nr-ir.h"

#include <neo.h>
#include <string.h>

typedef struct {
  GtkWindow  *win;
  JsonObject *ui;
  GtkWidget  *stack, *title, *back_btn, *add_btn, *record_btn, *wait_btn, *delete_btn, *snackbar;
  GtkWidget  *home_box, *grid, *remote_empty;
  GtkWidget  *brand_entry, *brand_list;
  GtkWidget  *search_entry, *search_list;
  GtkWidget  *scan_count, *scan_question, *yes_btn, *next_btn, *resend_btn;
  GStrv       brands;
  char       *page, *remote, *scan_id, *scan_brand;
  guint       settle_id, resend_id;
  gboolean    recording;
  JsonArray  *steps;
} Win;

typedef void (*NameDone) (Win *self, const char *text, gpointer data);

static void show_page (Win *self, const char *page);
static void open_remote (Win *self, const char *name);
static void home_fill (Win *self);

/* --- data: ui.json */

static const char *
T (Win *self, const char *key)
{
  g_auto(GStrv) parts = g_strsplit (key, ".", -1);
  JsonObject *o = self->ui;

  for (guint i = 0; o && parts[i]; i++)
    {
      if (!json_object_has_member (o, parts[i]))
        return key;
      if (!parts[i + 1])
        return json_object_get_string_member (o, parts[i]);
      o = json_object_get_object_member (o, parts[i]);
    }
  return key;
}

static double
conf_double (Win *self, const char *key)
{
  return json_object_get_double_member (json_object_get_object_member (self->ui, "conf"), key);
}

/* --- small widgets */

static GtkWidget *
label (const char *text, const char *css)
{
  GtkWidget *l = gtk_label_new (text);

  gtk_label_set_xalign (GTK_LABEL (l), 0);
  gtk_label_set_wrap (GTK_LABEL (l), TRUE);
  if (css)
    gtk_widget_add_css_class (l, css);
  return l;
}

static GtkWidget *
card (void)
{
  GtkWidget *c = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);

  gtk_widget_add_css_class (c, "neo-card");
  gtk_widget_add_css_class (c, "nr-card");
  return c;
}

/* a tappable row; `name` is kept on it ("nr-name") */
static GtkWidget *
row (const char *title, const char *sub, const char *name)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
  GtkWidget *pressable = neo_pressable_new (box);

  gtk_box_append (GTK_BOX (box), label (title, "nr-row-title"));
  if (sub && *sub)
    gtk_box_append (GTK_BOX (box), label (sub, "nr-sub"));
  gtk_widget_add_css_class (pressable, "nr-row");
  neo_pressable_set_ripple (NEO_PRESSABLE (pressable), NEO_RIPPLE_BOUNDED, 0);
  g_object_set_data_full (G_OBJECT (pressable), "nr-name", g_strdup (name), g_free);
  return pressable;
}

static const char *
name_of (GtkWidget *widget)
{
  return g_object_get_data (G_OBJECT (widget), "nr-name");
}

static GtkWidget *
scroller (GtkWidget *child)
{
  GtkWidget *s = gtk_scrolled_window_new ();

  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (s), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (s), child);
  gtk_widget_set_vexpand (s, TRUE);
  return s;
}

static void
clear (GtkWidget *container)
{
  GtkWidget *child;

  if (GTK_IS_BOX (container))
    while ((child = gtk_widget_get_first_child (container)))
      gtk_box_remove (GTK_BOX (container), child);
  else if (GTK_IS_FLOW_BOX (container))
    gtk_flow_box_remove_all (GTK_FLOW_BOX (container));
}

static void
toast (Win *self, const char *text, gboolean long_)
{
  neo_snackbar_show (NEO_SNACKBAR (self->snackbar), text, NULL, NULL, NULL,
                     long_ ? NEO_SNACKBAR_LONG : NEO_SNACKBAR_SHORT);
}

static void
toastf (Win *self, const char *key, const char *arg)
{
  g_autofree char *text = g_strdup_printf (T (self, key), arg);

  toast (self, text, FALSE);
}

/* a call's error, else FALSE */
static gboolean
failed (Win *self, const GError *error)
{
  if (!error)
    return FALSE;
  if (g_error_matches (error, G_DBUS_ERROR, G_DBUS_ERROR_SERVICE_UNKNOWN) ||
      g_error_matches (error, G_DBUS_ERROR, G_DBUS_ERROR_NO_REPLY))
    {
      g_autofree char *text = g_strdup_printf (T (self, "errors.service"), error->message);

      toast (self, text, TRUE);
    }
  else
    toast (self, error->message, TRUE);
  return TRUE;
}

static char *
json_text (JsonNode *node)
{
  return json_to_string (node, FALSE);
}

/* --- the name dialog */

typedef struct {
  Win          *self;
  GtkWidget    *entry;
  NameDone      done;
  gpointer      data;
  GDestroyNotify free;
} NameAsk;

static void
name_answered (NeoDialog *dialog, int response, gpointer user_data)
{
  NameAsk *ask = user_data;
  g_autofree char *text = g_strstrip (g_strdup (gtk_editable_get_text (GTK_EDITABLE (ask->entry))));

  if (response == NEO_RESPONSE_OK && *text)
    ask->done (ask->self, text, ask->data);
  if (ask->free)
    ask->free (ask->data);
  g_free (ask);
}

static void
ask_name (Win *self, const char *title, const char *hint, const char *initial, NameDone done, gpointer data,
          GDestroyNotify free_data)
{
  NeoDialog *dialog = neo_dialog_new (title);
  NameAsk *ask = g_new0 (NameAsk, 1);

  ask->self = self;
  ask->done = done;
  ask->data = data;
  ask->free = free_data;
  ask->entry = gtk_entry_new ();
  gtk_entry_set_placeholder_text (GTK_ENTRY (ask->entry), hint);
  if (initial)
    gtk_editable_set_text (GTK_EDITABLE (ask->entry), initial);
  neo_dialog_set_content (dialog, ask->entry);
  neo_dialog_add_button (dialog, T (self, "name.cancel"), NEO_RESPONSE_CANCEL);
  neo_dialog_add_button (dialog, T (self, "name.ok"), NEO_RESPONSE_OK);
  g_signal_connect (dialog, "response", G_CALLBACK (name_answered), ask);
  neo_dialog_present (dialog, GTK_WIDGET (self->win));
}

/* --- home: the remotes and the macros */

static void
on_remote_row (GtkWidget *row_widget, gpointer data)
{
  open_remote (data, name_of (row_widget));
}

static void
macro_ran (JsonNode *result, const GError *error, gpointer data)
{
  Win *self = data;

  if (!failed (self, error))
    toastf (self, "home.macro_done", json_object_get_string_member (json_node_get_object (result), "macro"));
}

static void
on_macro_row (GtkWidget *row_widget, gpointer data)
{
  if (g_object_steal_data (G_OBJECT (row_widget), "nr-long"))
    return;
  nr_ir_call ("RunMacro", g_variant_new ("(s)", name_of (row_widget)), macro_ran, data);
}

static void
macro_deleted (JsonNode *result, const GError *error, gpointer data)
{
  Win *self = data;

  if (failed (self, error))
    return;
  toastf (self, "home.macro_deleted", json_object_get_string_member (json_node_get_object (result), "deleted"));
  home_fill (self);
}

static void
macro_delete_answered (NeoDialog *dialog, int response, gpointer data)
{
  Win *self = g_object_get_data (G_OBJECT (dialog), "nr-win");

  if (response == NEO_RESPONSE_OK)
    nr_ir_call ("DeleteMacro", g_variant_new ("(s)", (char *) data), macro_deleted, self);
  g_free (data);
}

static void
on_macro_long (GtkGestureLongPress *gesture, double x, double y, gpointer data)
{
  Win *self = data;
  GtkWidget *row_widget = gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (gesture));
  NeoDialog *dialog = neo_dialog_new (name_of (row_widget));

  g_object_set_data (G_OBJECT (row_widget), "nr-long", GINT_TO_POINTER (1));
  g_object_set_data (G_OBJECT (dialog), "nr-win", self);
  neo_dialog_add_button (dialog, T (self, "remote.cancel"), NEO_RESPONSE_CANCEL);
  neo_dialog_add_button (dialog, T (self, "remote.delete"), NEO_RESPONSE_OK);
  g_signal_connect (dialog, "response", G_CALLBACK (macro_delete_answered), g_strdup (name_of (row_widget)));
  neo_dialog_present (dialog, GTK_WIDGET (self->win));
}

static void
macros_got (JsonNode *result, const GError *error, gpointer data)
{
  Win *self = data;
  JsonObject *macros;
  g_autoptr(GList) names = NULL;
  GtkWidget *c;

  if (failed (self, error))
    return;
  macros = json_node_get_object (result);
  names = json_object_get_members (macros);
  if (!names)
    return;
  gtk_box_append (GTK_BOX (self->home_box), label (T (self, "home.macros"), "nr-group"));
  c = card ();
  for (GList *l = names; l; l = l->next)
    {
      g_autofree char *sub = g_strdup_printf (T (self, "home.steps"),
                                              json_array_get_length (json_object_get_array_member (macros, l->data)));
      GtkWidget *r = row (l->data, sub, l->data);
      GtkGesture *lp = gtk_gesture_long_press_new ();

      g_signal_connect (r, "clicked", G_CALLBACK (on_macro_row), self);
      g_signal_connect (lp, "pressed", G_CALLBACK (on_macro_long), self);
      gtk_widget_add_controller (r, GTK_EVENT_CONTROLLER (lp));
      gtk_box_append (GTK_BOX (c), r);
    }
  gtk_box_append (GTK_BOX (self->home_box), c);
}

static void
remotes_got (JsonNode *result, const GError *error, gpointer data)
{
  Win *self = data;
  JsonArray *remotes;
  GtkWidget *c;

  clear (self->home_box);
  if (failed (self, error))
    return;
  remotes = json_node_get_array (result);
  gtk_box_append (GTK_BOX (self->home_box), label (T (self, "home.remotes"), "nr-group"));
  if (json_array_get_length (remotes) == 0)
    {
      GtkWidget *empty = label (T (self, "home.no_remotes"), "nr-sub");

      gtk_widget_add_css_class (empty, "nr-empty");
      gtk_box_append (GTK_BOX (self->home_box), empty);
    }
  else
    {
      c = card ();
      for (guint i = 0; i < json_array_get_length (remotes); i++)
        {
          JsonObject *r = json_array_get_object_element (remotes, i);
          const char *name = json_object_get_string_member (r, "remote");
          g_autofree char *sub = NULL;
          GtkWidget *w;

          if (json_object_has_member (r, "buttons"))
            sub = g_strdup_printf (T (self, "home.buttons"),
                                   json_array_get_length (json_object_get_array_member (r, "buttons")));
          else
            sub = g_strdup (json_object_get_string_member (r, "error"));
          w = row (name, sub, name);
          g_signal_connect (w, "clicked", G_CALLBACK (on_remote_row), self);
          gtk_box_append (GTK_BOX (c), w);
        }
      gtk_box_append (GTK_BOX (self->home_box), c);
    }
  nr_ir_call ("Macros", NULL, macros_got, self);
}

static void
home_fill (Win *self)
{
  nr_ir_call ("Remotes", NULL, remotes_got, self);
}

/* --- a remote: its buttons */

static gboolean
is_power (const char *name)
{
  g_autofree char *low = g_ascii_strdown (name, -1);

  return strstr (low, "power") != NULL;
}

static void
pressed_done (JsonNode *result, const GError *error, gpointer data)
{
  Win *self = data;

  if (!failed (self, error) && !self->recording)
    toastf (self, "remote.sent", json_object_get_string_member (json_node_get_object (result), "button"));
}

static void
record_toast (Win *self)
{
  g_autofree char *text = g_strdup_printf (T (self, "remote.record_step"), json_array_get_length (self->steps));

  toast (self, text, FALSE);
}

static void
on_key (GtkWidget *key, gpointer data)
{
  Win *self = data;
  const char *button = name_of (key);

  if (g_object_steal_data (G_OBJECT (key), "nr-long"))
    return;
  nr_ir_call ("Press", g_variant_new ("(ssi)", self->remote, button, 1), pressed_done, self);
  if (self->recording)
    {
      JsonObject *step = json_object_new ();

      json_object_set_string_member (step, "remote", self->remote);
      json_object_set_string_member (step, "button", button);
      json_array_add_object_element (self->steps, step);
      record_toast (self);
    }
}

static void
button_deleted (JsonNode *result, const GError *error, gpointer data)
{
  Win *self = data;

  if (!failed (self, error))
    open_remote (self, self->remote);
}

static void
key_answered (NeoDialog *dialog, int response, gpointer data)
{
  Win *self = g_object_get_data (G_OBJECT (dialog), "nr-win");

  if (response == NEO_RESPONSE_OK)
    nr_ir_call ("DeleteButton", g_variant_new ("(ss)", self->remote, (char *) data), button_deleted, self);
  g_free (data);
}

static void
on_key_long (GtkGestureLongPress *gesture, double x, double y, gpointer data)
{
  Win *self = data;
  GtkWidget *key = gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (gesture));
  g_autofree char *title = g_strdup_printf (T (self, "remote.edit_title"), name_of (key));
  NeoDialog *dialog = neo_dialog_new (title);

  g_object_set_data (G_OBJECT (key), "nr-long", GINT_TO_POINTER (1));
  g_object_set_data (G_OBJECT (dialog), "nr-win", self);
  neo_dialog_add_button (dialog, T (self, "remote.cancel"), NEO_RESPONSE_CANCEL);
  neo_dialog_add_button (dialog, T (self, "remote.delete_button"), NEO_RESPONSE_OK);
  g_signal_connect (dialog, "response", G_CALLBACK (key_answered), g_strdup (name_of (key)));
  neo_dialog_present (dialog, GTK_WIDGET (self->win));
}

static void
remote_got (JsonNode *result, const GError *error, gpointer data)
{
  Win *self = data;
  JsonArray *remotes;
  JsonArray *buttons = NULL;

  clear (self->grid);
  if (failed (self, error))
    return;
  remotes = json_node_get_array (result);
  for (guint i = 0; i < json_array_get_length (remotes); i++)
    {
      JsonObject *r = json_array_get_object_element (remotes, i);

      if (g_strcmp0 (json_object_get_string_member (r, "remote"), self->remote) == 0 &&
          json_object_has_member (r, "buttons"))
        buttons = json_object_get_array_member (r, "buttons");
    }
  gtk_widget_set_visible (self->remote_empty, !buttons || json_array_get_length (buttons) == 0);
  for (guint i = 0; buttons && i < json_array_get_length (buttons); i++)
    {
      const char *name = json_array_get_string_element (buttons, i);
      GtkWidget *text = gtk_label_new (name);
      GtkWidget *key = neo_pressable_new (text);
      GtkGesture *lp = gtk_gesture_long_press_new ();

      gtk_label_set_wrap (GTK_LABEL (text), TRUE);
      gtk_label_set_justify (GTK_LABEL (text), GTK_JUSTIFY_CENTER);
      gtk_widget_add_css_class (key, "nr-key");
      if (is_power (name))
        gtk_widget_add_css_class (key, "nr-power");
      neo_pressable_set_ripple (NEO_PRESSABLE (key), NEO_RIPPLE_BOUNDED, 0);
      g_object_set_data_full (G_OBJECT (key), "nr-name", g_strdup (name), g_free);
      g_signal_connect (key, "clicked", G_CALLBACK (on_key), self);
      g_signal_connect (lp, "pressed", G_CALLBACK (on_key_long), self);
      gtk_widget_add_controller (key, GTK_EVENT_CONTROLLER (lp));
      gtk_flow_box_append (GTK_FLOW_BOX (self->grid), key);
    }
}

static void
open_remote (Win *self, const char *name)
{
  g_autofree char *keep = g_strdup (name);   /* `name` may be self->remote itself */

  g_free (self->remote);
  self->remote = g_steal_pointer (&keep);
  show_page (self, "remote");
  nr_ir_call ("Remotes", NULL, remote_got, self);
}

/* the new-button dialog */

static GtkWidget *
form_entry (GtkWidget *form, const char *hint)
{
  GtkWidget *entry = gtk_entry_new ();

  gtk_entry_set_placeholder_text (GTK_ENTRY (entry), hint);
  gtk_box_append (GTK_BOX (form), entry);
  return entry;
}

static const char *
entry_text (GtkWidget *form, const char *key)
{
  return gtk_editable_get_text (GTK_EDITABLE (g_object_get_data (G_OBJECT (form), key)));
}

static void
button_saved (JsonNode *result, const GError *error, gpointer data)
{
  Win *self = data;

  if (!failed (self, error))
    open_remote (self, self->remote);
}

static void
button_answered (NeoDialog *dialog, int response, gpointer data)
{
  Win *self = g_object_get_data (G_OBJECT (dialog), "nr-win");
  GtkWidget *form = data;
  g_autoptr(JsonNode) node = NULL;
  g_autofree char *text = NULL;
  JsonObject *b;
  const char *protocol;

  if (response != NEO_RESPONSE_OK)
    return;
  b = json_object_new ();
  json_object_set_string_member (b, "name", entry_text (form, "name"));
  protocol = entry_text (form, "protocol");
  if (*protocol)
    {
      json_object_set_string_member (b, "type", "parsed");
      json_object_set_string_member (b, "protocol", protocol);
      json_object_set_int_member (b, "address", g_ascii_strtoll (entry_text (form, "address"), NULL, 0));
      json_object_set_int_member (b, "command", g_ascii_strtoll (entry_text (form, "command"), NULL, 0));
    }
  else
    {
      g_auto(GStrv) values = g_strsplit_set (entry_text (form, "raw"), " ,\t", -1);
      JsonArray *raw = json_array_new ();

      for (guint i = 0; values[i]; i++)
        if (*values[i])
          json_array_add_int_element (raw, g_ascii_strtoll (values[i], NULL, 10));
      json_object_set_string_member (b, "type", "raw");
      json_object_set_int_member (b, "frequency", g_ascii_strtoll (entry_text (form, "frequency"), NULL, 10));
      json_object_set_array_member (b, "data", raw);
    }
  node = json_node_init_object (json_node_alloc (), b);
  json_object_unref (b);
  text = json_text (node);
  nr_ir_call ("SetButton", g_variant_new ("(ss)", self->remote, text), button_saved, self);
}

static void
ask_button (Win *self)
{
  NeoDialog *dialog = neo_dialog_new (T (self, "button.title"));
  GtkWidget *form = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  const char *keys[] = { "name", "protocol", "address", "command", "frequency", "raw" };

  gtk_widget_add_css_class (form, "nr-form");
  for (guint i = 0; i < G_N_ELEMENTS (keys); i++)
    {
      g_autofree char *hint_key = g_strconcat ("button.", keys[i], NULL);

      g_object_set_data (G_OBJECT (form), keys[i], form_entry (form, T (self, hint_key)));
    }
  neo_dialog_set_content (dialog, form);
  g_object_set_data (G_OBJECT (dialog), "nr-win", self);
  neo_dialog_add_button (dialog, T (self, "button.cancel"), NEO_RESPONSE_CANCEL);
  neo_dialog_add_button (dialog, T (self, "button.ok"), NEO_RESPONSE_OK);
  g_signal_connect (dialog, "response", G_CALLBACK (button_answered), form);
  neo_dialog_present (dialog, GTK_WIDGET (self->win));
}

/* recording a macro */

static void
macro_saved (JsonNode *result, const GError *error, gpointer data)
{
  Win *self = data;

  if (!failed (self, error))
    toastf (self, "home.macro_done", json_object_get_string_member (json_node_get_object (result), "macro"));
}

static void
macro_named (Win *self, const char *name, gpointer data)
{
  JsonArray *steps = data;
  g_autoptr(JsonNode) node = json_node_init_array (json_node_alloc (), steps);
  g_autofree char *text = json_text (node);

  nr_ir_call ("SaveMacro", g_variant_new ("(ss)", name, text), macro_saved, self);
}

static void
set_recording (Win *self, gboolean on)
{
  self->recording = on;
  neo_fab_set_icon (NEO_PRESSABLE (self->record_btn), T (self, on ? "icons.stop" : "icons.record"));
  if (on)
    gtk_widget_add_css_class (self->record_btn, "nr-recording");
  else
    gtk_widget_remove_css_class (self->record_btn, "nr-recording");
  gtk_widget_set_visible (self->wait_btn, on);
}

static void
on_record (GtkWidget *button, gpointer data)
{
  Win *self = data;

  if (!self->recording)
    {
      g_clear_pointer (&self->steps, json_array_unref);
      self->steps = json_array_new ();
      set_recording (self, TRUE);
      toast (self, T (self, "remote.record_on"), TRUE);
      return;
    }
  set_recording (self, FALSE);
  if (json_array_get_length (self->steps) == 0)
    {
      toast (self, T (self, "remote.record_empty"), FALSE);
      return;
    }
  ask_name (self, T (self, "name.title"), T (self, "name.macro_hint"), NULL, macro_named,
            json_array_ref (self->steps), (GDestroyNotify) json_array_unref);
}

static void
on_wait (GtkWidget *button, gpointer data)
{
  Win *self = data;
  JsonObject *step = json_object_new ();

  json_object_set_double_member (step, "wait", conf_double (self, "macro_wait_s"));
  json_array_add_object_element (self->steps, step);
  record_toast (self);
}

/* deleting a remote */

static void
remote_deleted (JsonNode *result, const GError *error, gpointer data)
{
  Win *self = data;

  if (!failed (self, error))
    show_page (self, "home");
}

static void
remote_delete_answered (NeoDialog *dialog, int response, gpointer data)
{
  Win *self = data;

  if (response == NEO_RESPONSE_OK)
    nr_ir_call ("DeleteRemote", g_variant_new ("(s)", self->remote), remote_deleted, self);
}

static void
on_delete (GtkWidget *button, gpointer data)
{
  Win *self = data;
  g_autofree char *title = g_strdup_printf (T (self, "remote.delete_remote"), self->remote);
  NeoDialog *dialog = neo_dialog_new (title);

  neo_dialog_add_button (dialog, T (self, "remote.cancel"), NEO_RESPONSE_CANCEL);
  neo_dialog_add_button (dialog, T (self, "remote.delete"), NEO_RESPONSE_OK);
  g_signal_connect (dialog, "response", G_CALLBACK (remote_delete_answered), self);
  neo_dialog_present (dialog, GTK_WIDGET (self->win));
}

/* --- the scan by brand */

static void scan_state (JsonNode *result, const GError *error, gpointer data);

static void
answers_sensitive (Win *self, gboolean on)
{
  gtk_widget_set_sensitive (self->yes_btn, on);
  gtk_widget_set_sensitive (self->next_btn, on);
  gtk_widget_set_sensitive (self->resend_btn, on);
}

static void
scan_timers_stop (Win *self)
{
  g_clear_handle_id (&self->settle_id, g_source_remove);
  g_clear_handle_id (&self->resend_id, g_source_remove);
}

static gboolean
on_settled (gpointer data)
{
  Win *self = data;

  self->settle_id = 0;
  answers_sensitive (self, TRUE);
  gtk_label_set_text (GTK_LABEL (self->scan_question), T (self, "scan.question"));
  return G_SOURCE_REMOVE;
}

static void
resend_ignored (JsonNode *result, const GError *error, gpointer data)
{
}

static gboolean
on_resend_due (gpointer data)
{
  Win *self = data;

  if (self->scan_id)
    nr_ir_call ("ScanResend", g_variant_new ("(s)", self->scan_id), resend_ignored, NULL);
  return G_SOURCE_CONTINUE;
}

static void
scan_saved (JsonNode *result, const GError *error, gpointer data)
{
  Win *self = data;
  const char *saved;

  if (failed (self, error))
    {
      answers_sensitive (self, TRUE);
      return;
    }
  saved = json_object_get_string_member (json_node_get_object (result), "saved");
  g_clear_pointer (&self->scan_id, g_free);
  toastf (self, "scan.saved", saved);
  open_remote (self, saved);
}

static void
scan_named (Win *self, const char *name, gpointer data)
{
  if (self->scan_id)
    nr_ir_call ("ScanHit", g_variant_new ("(ss)", self->scan_id, name), scan_saved, self);
}

static void
on_yes (GtkWidget *button, gpointer data)
{
  Win *self = data;

  scan_timers_stop (self);
  ask_name (self, T (self, "name.title"), T (self, "name.remote_hint"), self->scan_brand, scan_named, NULL, NULL);
}

static void
scan_send (Win *self, const char *method)
{
  scan_timers_stop (self);
  answers_sensitive (self, FALSE);
  gtk_label_set_text (GTK_LABEL (self->scan_question), T (self, "scan.wait"));
  nr_ir_call (method, g_variant_new ("(s)", self->scan_id), scan_state, self);
}

static void
on_next (GtkWidget *button, gpointer data)
{
  scan_send (data, "ScanNext");
}

static void
on_resend (GtkWidget *button, gpointer data)
{
  scan_send (data, "ScanResend");
}

static void
scan_state (JsonNode *result, const GError *error, gpointer data)
{
  Win *self = data;
  JsonObject *s;
  g_autofree char *count = NULL;
  double resend_s;

  if (failed (self, error))
    {
      answers_sensitive (self, TRUE);
      return;
    }
  s = json_node_get_object (result);
  if (json_object_get_boolean_member (s, "done"))
    {
      gtk_label_set_text (GTK_LABEL (self->scan_count), "");
      gtk_label_set_text (GTK_LABEL (self->scan_question), T (self, "scan.none"));
      answers_sensitive (self, FALSE);
      return;
    }
  count = g_strdup_printf (T (self, "scan.count"), (int) json_object_get_int_member (s, "index"),
                           (int) json_object_get_int_member (s, "total"));
  gtk_label_set_text (GTK_LABEL (self->scan_count), count);
  /* the timing is minerva-ir's ([scan] settle_s, resend_s), handed over with every step */
  self->settle_id = g_timeout_add ((guint) (json_object_get_double_member (s, "settle_s") * 1000), on_settled, self);
  resend_s = json_object_get_double_member (s, "resend_s");
  if (resend_s > 0)
    self->resend_id = g_timeout_add ((guint) (resend_s * 1000), on_resend_due, self);
}

static void
scan_started (JsonNode *result, const GError *error, gpointer data)
{
  Win *self = data;

  if (failed (self, error))
    return;
  g_free (self->scan_id);
  self->scan_id = g_strdup (json_object_get_string_member (json_node_get_object (result), "scan"));
  show_page (self, "scan");
  scan_send (self, "ScanNext");
}

static void
on_brand (GtkWidget *row_widget, gpointer data)
{
  Win *self = data;

  g_free (self->scan_brand);
  self->scan_brand = g_strdup (name_of (row_widget));
  nr_ir_call ("ScanStart", g_variant_new ("(ss)", self->scan_brand, ""), scan_started, self);
}

static void
brands_filter (Win *self)
{
  g_autofree char *needle = g_utf8_casefold (gtk_editable_get_text (GTK_EDITABLE (self->brand_entry)), -1);
  GtkWidget *c = card ();
  guint shown = 0;

  clear (self->brand_list);
  for (guint i = 0; self->brands && self->brands[i]; i++)
    {
      g_autofree char *hay = g_utf8_casefold (self->brands[i], -1);
      g_autofree char *title = g_strdelimit (g_strdup (self->brands[i]), "_", ' ');
      GtkWidget *r;

      if (*needle && !strstr (hay, needle))
        continue;
      r = row (title, NULL, self->brands[i]);
      g_signal_connect (r, "clicked", G_CALLBACK (on_brand), self);
      gtk_box_append (GTK_BOX (c), r);
      shown++;
    }
  if (shown)
    gtk_box_append (GTK_BOX (self->brand_list), c);
  else
    gtk_box_append (GTK_BOX (self->brand_list), label (T (self, "brands.none"), "nr-sub"));
}

static void
on_brand_filter (GtkEditable *entry, gpointer data)
{
  brands_filter (data);
}

static void
brands_got (JsonNode *result, const GError *error, gpointer data)
{
  Win *self = data;
  JsonArray *list;

  if (failed (self, error))
    return;
  list = json_node_get_array (result);
  g_strfreev (self->brands);
  self->brands = g_new0 (char *, json_array_get_length (list) + 1);
  for (guint i = 0; i < json_array_get_length (list); i++)
    self->brands[i] = g_strdup (json_array_get_string_element (list, i));
  brands_filter (self);
}

/* --- the database search */

static void
imported (JsonNode *result, const GError *error, gpointer data)
{
  Win *self = data;
  const char *name;

  if (failed (self, error))
    return;
  name = json_object_get_string_member (json_node_get_object (result), "remote");
  toastf (self, "search.imported", name);
  open_remote (self, name);
}

static void
import_named (Win *self, const char *name, gpointer data)
{
  nr_ir_call ("Import", g_variant_new ("(ss)", (char *) data, name), imported, self);
}

static void
on_result (GtkWidget *row_widget, gpointer data)
{
  Win *self = data;
  g_autofree char *base = g_path_get_basename (name_of (row_widget));
  char *dot = strrchr (base, '.');

  if (dot)
    *dot = '\0';
  ask_name (self, T (self, "name.title"), T (self, "name.remote_hint"), base, import_named,
            g_strdup (name_of (row_widget)), g_free);
}

static void
search_got (JsonNode *result, const GError *error, gpointer data)
{
  Win *self = data;
  JsonArray *list;
  GtkWidget *c;

  clear (self->search_list);
  if (failed (self, error))
    return;
  list = json_node_get_array (result);
  if (json_array_get_length (list) == 0)
    {
      gtk_box_append (GTK_BOX (self->search_list), label (T (self, "search.none"), "nr-sub"));
      return;
    }
  c = card ();
  for (guint i = 0; i < json_array_get_length (list); i++)
    {
      const char *path = json_array_get_string_element (list, i);
      g_autofree char *base = g_path_get_basename (path);
      g_autofree char *dir = g_path_get_dirname (path);
      GtkWidget *r = row (base, dir, path);

      g_signal_connect (r, "clicked", G_CALLBACK (on_result), self);
      gtk_box_append (GTK_BOX (c), r);
    }
  gtk_box_append (GTK_BOX (self->search_list), c);
}

static void
on_search (GtkEntry *entry, gpointer data)
{
  const char *query = gtk_editable_get_text (GTK_EDITABLE (entry));

  if (*query)
    nr_ir_call ("Search", g_variant_new ("(ss)", query, ""), search_got, data);
}

/* --- adding a remote */

static void
remote_created (JsonNode *result, const GError *error, gpointer data)
{
  Win *self = data;

  if (!failed (self, error))
    open_remote (self, json_object_get_string_member (json_node_get_object (result), "remote"));
}

static void
empty_named (Win *self, const char *name, gpointer data)
{
  nr_ir_call ("NewRemote", g_variant_new ("(s)", name), remote_created, self);
}

enum { ADD_SCAN = 10, ADD_DB, ADD_EMPTY };

static void
add_answered (NeoDialog *dialog, int response, gpointer data)
{
  Win *self = data;

  if (response == ADD_SCAN)
    show_page (self, "brands");
  else if (response == ADD_DB)
    show_page (self, "search");
  else if (response == ADD_EMPTY)
    ask_name (self, T (self, "name.title"), T (self, "name.remote_hint"), NULL, empty_named, NULL, NULL);
}

static void
on_add (GtkWidget *button, gpointer data)
{
  Win *self = data;
  NeoDialog *dialog;

  if (g_strcmp0 (self->page, "remote") == 0)
    {
      ask_button (self);
      return;
    }
  dialog = neo_dialog_new (T (self, "add.title"));
  neo_dialog_add_button (dialog, T (self, "add.scan"), ADD_SCAN);
  neo_dialog_add_button (dialog, T (self, "add.db"), ADD_DB);
  neo_dialog_add_button (dialog, T (self, "add.empty"), ADD_EMPTY);
  neo_dialog_add_button (dialog, T (self, "add.cancel"), NEO_RESPONSE_CANCEL);
  g_signal_connect (dialog, "response", G_CALLBACK (add_answered), self);
  neo_dialog_present (dialog, GTK_WIDGET (self->win));
}

/* --- pages and the bar */

static void
show_page (Win *self, const char *page)
{
  gboolean home = g_str_equal (page, "home"), remote = g_str_equal (page, "remote");

  if (!g_str_equal (page, "scan"))
    scan_timers_stop (self);
  g_free (self->page);
  self->page = g_strdup (page);
  gtk_stack_set_visible_child_name (GTK_STACK (self->stack), page);
  gtk_widget_set_visible (self->back_btn, !home);
  gtk_widget_set_visible (self->add_btn, home || remote);
  gtk_widget_set_visible (self->record_btn, remote || self->recording);
  gtk_widget_set_visible (self->wait_btn, self->recording);
  gtk_widget_set_visible (self->delete_btn, remote);
  if (home)
    {
      gtk_label_set_text (GTK_LABEL (self->title), T (self, "app.name"));
      home_fill (self);
    }
  else if (remote)
    gtk_label_set_text (GTK_LABEL (self->title), self->remote);
  else if (g_str_equal (page, "brands"))
    {
      gtk_label_set_text (GTK_LABEL (self->title), T (self, "brands.title"));
      nr_ir_call ("Brands", g_variant_new ("(s)", ""), brands_got, self);
    }
  else if (g_str_equal (page, "scan"))
    {
      gtk_label_set_text (GTK_LABEL (self->title), self->scan_brand);
      gtk_label_set_text (GTK_LABEL (self->scan_count), "");
      answers_sensitive (self, FALSE);
    }
  else if (g_str_equal (page, "search"))
    {
      gtk_label_set_text (GTK_LABEL (self->title), T (self, "search.title"));
      gtk_widget_grab_focus (self->search_entry);
    }
}

static void
on_back (GtkWidget *button, gpointer data)
{
  Win *self = data;

  if (g_strcmp0 (self->page, "scan") == 0)
    {
      g_clear_pointer (&self->scan_id, g_free);
      show_page (self, "brands");
    }
  else
    show_page (self, "home");
}

static GtkWidget *
answer_button (const char *text, const char *css, GCallback cb, Win *self)
{
  GtkWidget *l = gtk_label_new (text);
  GtkWidget *b = neo_pressable_new (l);

  gtk_widget_add_css_class (b, "nr-answer");
  if (css)
    gtk_widget_add_css_class (b, css);
  neo_pressable_set_ripple (NEO_PRESSABLE (b), NEO_RIPPLE_BOUNDED, 0);
  g_signal_connect (b, "clicked", cb, self);
  return b;
}

static GtkWidget *
scan_page (Win *self)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 16);

  gtk_widget_add_css_class (box, "nr-scan");
  gtk_box_append (GTK_BOX (box), label (T (self, "scan.point"), "nr-sub"));
  self->scan_count = label ("", "nr-scan-count");
  self->scan_question = label ("", "nr-scan-question");
  gtk_box_append (GTK_BOX (box), self->scan_count);
  gtk_box_append (GTK_BOX (box), self->scan_question);
  self->yes_btn = answer_button (T (self, "scan.yes"), "nr-yes", G_CALLBACK (on_yes), self);
  self->next_btn = answer_button (T (self, "scan.next"), NULL, G_CALLBACK (on_next), self);
  self->resend_btn = answer_button (T (self, "scan.resend"), NULL, G_CALLBACK (on_resend), self);
  gtk_box_append (GTK_BOX (box), self->yes_btn);
  gtk_box_append (GTK_BOX (box), self->next_btn);
  gtk_box_append (GTK_BOX (box), self->resend_btn);
  return scroller (box);
}

static GtkWidget *
entry_page (Win *self, const char *hint, GtkWidget **entry, GtkWidget **list)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);

  *entry = gtk_search_entry_new ();
  gtk_search_entry_set_placeholder_text (GTK_SEARCH_ENTRY (*entry), hint);
  gtk_widget_add_css_class (*entry, "nr-entry");
  *list = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_append (GTK_BOX (box), *entry);
  gtk_box_append (GTK_BOX (box), scroller (*list));
  return box;
}

static GtkWidget *
bar_button (Win *self, const char *icon, GCallback cb)
{
  GtkWidget *b = neo_icon_button_new (T (self, icon));

  g_signal_connect (b, "clicked", cb, self);
  return b;
}

static void
win_free (Win *self)
{
  scan_timers_stop (self);
  g_strfreev (self->brands);
  g_free (self->page);
  g_free (self->remote);
  g_free (self->scan_id);
  g_free (self->scan_brand);
  g_clear_pointer (&self->steps, json_array_unref);
  g_free (self);
}

GtkWidget *
nr_window_new (GtkApplication *app, JsonObject *ui)
{
  Win *self = g_new0 (Win, 1);
  GtkWidget *window = neo_window_new (app);
  GtkWidget *bar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 2);
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *remote_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *overlay;

  self->ui = ui;
  self->win = GTK_WINDOW (window);
  gtk_widget_add_css_class (window, "nr");
  g_object_set_data_full (G_OBJECT (window), "nr-win", self, (GDestroyNotify) win_free);

  gtk_widget_add_css_class (bar, "neo-app-bar");
  /* Back carries the desktop's own word for "Back": the shell's Back gesture looks for it, in any locale */
  self->back_btn = bar_button (self, "icons.back", G_CALLBACK (on_back));
  gtk_accessible_update_property (GTK_ACCESSIBLE (self->back_btn), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                  g_dgettext ("gtk40", "Back"), -1);
  gtk_box_append (GTK_BOX (bar), self->back_btn);
  self->title = label (T (self, "app.name"), "neo-app-bar-title");
  gtk_widget_set_hexpand (self->title, TRUE);
  gtk_label_set_ellipsize (GTK_LABEL (self->title), PANGO_ELLIPSIZE_END);
  gtk_box_append (GTK_BOX (bar), self->title);
  self->wait_btn = neo_text_button_new (T (self, "remote.record_wait"));
  g_signal_connect (self->wait_btn, "clicked", G_CALLBACK (on_wait), self);
  self->record_btn = bar_button (self, "icons.record", G_CALLBACK (on_record));
  self->delete_btn = bar_button (self, "icons.delete", G_CALLBACK (on_delete));
  self->add_btn = bar_button (self, "icons.add", G_CALLBACK (on_add));
  gtk_box_append (GTK_BOX (bar), self->wait_btn);
  gtk_box_append (GTK_BOX (bar), self->record_btn);
  gtk_box_append (GTK_BOX (bar), self->delete_btn);
  gtk_box_append (GTK_BOX (bar), self->add_btn);

  self->stack = gtk_stack_new ();
  gtk_widget_set_vexpand (self->stack, TRUE);
  self->home_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_stack_add_named (GTK_STACK (self->stack), scroller (self->home_box), "home");

  self->grid = gtk_flow_box_new ();
  gtk_flow_box_set_selection_mode (GTK_FLOW_BOX (self->grid), GTK_SELECTION_NONE);
  gtk_flow_box_set_homogeneous (GTK_FLOW_BOX (self->grid), TRUE);
  gtk_flow_box_set_min_children_per_line (GTK_FLOW_BOX (self->grid), (guint) conf_double (self, "grid_columns"));
  gtk_flow_box_set_max_children_per_line (GTK_FLOW_BOX (self->grid), (guint) conf_double (self, "grid_columns"));
  gtk_widget_add_css_class (self->grid, "nr-grid");
  self->remote_empty = label (T (self, "remote.empty"), "nr-sub");
  gtk_widget_add_css_class (self->remote_empty, "nr-empty");
  gtk_box_append (GTK_BOX (remote_box), self->remote_empty);
  gtk_box_append (GTK_BOX (remote_box), self->grid);
  gtk_stack_add_named (GTK_STACK (self->stack), scroller (remote_box), "remote");

  gtk_stack_add_named (GTK_STACK (self->stack),
                       entry_page (self, T (self, "brands.filter"), &self->brand_entry, &self->brand_list), "brands");
  g_signal_connect (self->brand_entry, "search-changed", G_CALLBACK (on_brand_filter), self);
  gtk_stack_add_named (GTK_STACK (self->stack), scan_page (self), "scan");
  gtk_stack_add_named (GTK_STACK (self->stack),
                       entry_page (self, T (self, "search.hint"), &self->search_entry, &self->search_list), "search");
  g_signal_connect (self->search_entry, "activate", G_CALLBACK (on_search), self);

  self->snackbar = neo_snackbar_new ();
  gtk_box_append (GTK_BOX (box), bar);
  gtk_box_append (GTK_BOX (box), self->stack);
  neo_window_set_content (self->win, box);
  overlay = GTK_WIDGET (neo_window_get_overlay (box));
  if (overlay)
    gtk_overlay_add_overlay (GTK_OVERLAY (overlay), self->snackbar);
  set_recording (self, FALSE);
  show_page (self, "home");
  return window;
}
