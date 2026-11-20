/* Neo Voice's window: see nv-window.h. */
#include "nv-window.h"
#include "nv-wave.h"

#include <neo.h>

typedef struct {
  NvStore      *store;
  GtkWindow    *win;
  GtkWidget    *box, *body, *stack, *side, *back_btn, *title, *snackbar;
  GtkWidget    *list_root, *detail_root, *status, *filters, *rows, *empty;
  /* the detail */
  GtkWidget    *d_title, *d_lines, *d_heard, *wave, *no_audio, *play_btn, *trim_save, *trim_reset, *labels, *note;
  GtkWidget    *pick_hint, *detail_box;
  GtkMediaStream *media;
  GBytes       *clip;
  JsonNode     *records;   /* the last list (an array) */
  JsonObject   *sel;       /* the selected record (inside records, or the reply of a change) */
  JsonNode     *sel_node;
  char         *sel_id, *want_id;
  char         *filter;    /* the id of filters_order */
  gboolean      landscape, laid_out, detail_open;
} Win;

static void load (Win *self);
static void show_detail (Win *self, JsonObject *record);

/* ---- small shapes ---- */

static GtkWidget *
label_new (const char *text, const char *css)
{
  GtkWidget *label = gtk_label_new (text ? text : "");

  gtk_label_set_xalign (GTK_LABEL (label), 0);
  gtk_label_set_wrap (GTK_LABEL (label), TRUE);
  if (css)
    gtk_widget_add_css_class (label, css);
  return label;
}

static const char *
str (JsonObject *o, const char *key)
{
  JsonNode *node = o ? json_object_get_member (o, key) : NULL;

  return node && JSON_NODE_HOLDS_VALUE (node) && json_node_get_value_type (node) == G_TYPE_STRING
         ? json_node_get_string (node) : "";
}

static gboolean
num (JsonObject *o, const char *key, double *out)
{
  JsonNode *node = o ? json_object_get_member (o, key) : NULL;

  if (!node || !JSON_NODE_HOLDS_VALUE (node) || json_node_get_value_type (node) == G_TYPE_STRING)
    return FALSE;
  *out = json_node_get_double (node);
  return TRUE;
}

/* a name of the ui file under `group` for this id ("reason.not_heard"); the id itself when it has none */
static const char *
named (Win *self, const char *group, const char *id)
{
  g_autofree char *key = g_strdup_printf ("%s.%s", group, id);
  const char *text = nv_ui (self->store, key);

  return g_strcmp0 (text, key) ? text : id;
}

static void
snack (Win *self, const char *text)
{
  neo_snackbar_show (NEO_SNACKBAR (self->snackbar), text, NULL, NULL, NULL, NEO_SNACKBAR_SHORT);
}

static void
snack_error (Win *self, const char *error)
{
  g_autofree char *text = g_strdup_printf (nv_ui (self->store, "snack.error"), error);

  snack (self, text);
}

/* the record's local time, "20/11 17:42:05" */
static char *
when (JsonObject *record)
{
  g_autoptr(GDateTime) at = g_date_time_new_from_iso8601 (str (record, "at"), NULL);
  g_autoptr(GDateTime) local = at ? g_date_time_to_local (at) : NULL;

  return local ? g_date_time_format (local, "%d/%m %H:%M:%S") : g_strdup (str (record, "id"));
}

static char *
score_text (JsonObject *record)
{
  double v = 0;

  if (num (record, "score_milli", &v))
    return g_strdup_printf ("%.2f", v / 1000.0);
  if (num (record, "score", &v))
    return g_strdup_printf ("%.2f", v);
  return g_strdup ("");
}

static char *
row_sub (Win *self, JsonObject *r)
{
  GString *s = g_string_new (named (self, "outcome", str (r, "outcome")));

  if (*str (r, "reason"))
    g_string_append_printf (s, " · %s", named (self, "reason", str (r, "reason")));
  g_string_append_printf (s, " · %s", named (self, "band", str (r, "speaker_band")));
  g_string_append_printf (s, " · %s", named (self, "label", str (r, "label")));
  return g_string_free (s, FALSE);
}

/* ---- the layout: rotation ---- */

static void
update_chrome (Win *self)
{
  gboolean detail = !self->landscape && self->detail_open;

  gtk_widget_set_visible (self->back_btn, detail);
  gtk_label_set_text (GTK_LABEL (self->title), nv_ui (self->store, "app.name"));
  if (!self->landscape)
    gtk_stack_set_visible_child (GTK_STACK (self->stack), detail ? self->detail_root : self->list_root);
}

static void
relayout (Win *self, gboolean landscape)
{
  if (self->laid_out && self->landscape == landscape)
    return;
  self->laid_out = TRUE;
  self->landscape = landscape;
  g_object_ref (self->list_root);
  g_object_ref (self->detail_root);
  for (GtkWidget *w = self->list_root; w; w = w == self->list_root ? self->detail_root : NULL)
    {
      GtkWidget *parent = gtk_widget_get_parent (w);

      if (parent == self->stack)
        gtk_stack_remove (GTK_STACK (self->stack), w);
      else if (parent)
        gtk_box_remove (GTK_BOX (parent), w);
    }
  if (landscape)
    {
      gtk_box_append (GTK_BOX (self->side), self->list_root);
      gtk_box_append (GTK_BOX (self->body), self->detail_root);
    }
  else
    {
      gtk_stack_add_named (GTK_STACK (self->stack), self->list_root, "list");
      gtk_stack_add_named (GTK_STACK (self->stack), self->detail_root, "detail");
    }
  g_object_unref (self->list_root);
  g_object_unref (self->detail_root);
  gtk_widget_set_visible (self->side, landscape);
  gtk_widget_set_visible (self->stack, !landscape);
  update_chrome (self);
}

static void
on_surface_layout (GdkSurface *surface, int width, int height, gpointer data)
{
  Win *self = data;
  double aspect = nv_conf_double (self->store, "landscape_aspect");

  relayout (self, aspect > 0 && height > 0 && width >= height * aspect);
}

static void
on_realize (GtkWidget *window, gpointer data)
{
  g_signal_connect (gtk_native_get_surface (GTK_NATIVE (window)), "layout", G_CALLBACK (on_surface_layout), data);
}

static void
back (Win *self)
{
  if (!self->landscape && self->detail_open)
    {
      self->detail_open = FALSE;
      if (self->media)
        gtk_media_stream_pause (self->media);
      update_chrome (self);
    }
  else
    gtk_window_close (self->win);
}

static void
on_back (GtkWidget *button, gpointer data)
{
  back (data);
}

static gboolean
on_back_action (GtkWidget *widget, GVariant *args, gpointer data)
{
  back (data);
  return TRUE;
}

/* ---- the list ---- */

static void
on_row (GtkWidget *row, gpointer data)
{
  Win *self = data;

  show_detail (self, g_object_get_data (G_OBJECT (row), "nv-record"));
}

static void
fill_rows (Win *self)
{
  JsonArray *array = self->records && JSON_NODE_HOLDS_ARRAY (self->records) ? json_node_get_array (self->records) : NULL;
  GtkWidget *child = NULL;
  guint n = array ? json_array_get_length (array) : 0;

  while ((child = gtk_widget_get_first_child (self->rows)))
    gtk_box_remove (GTK_BOX (self->rows), child);
  gtk_widget_set_visible (self->empty, n == 0);
  for (guint i = 0; i < n; i++)
    {
      JsonObject *r = json_array_get_object_element (array, i);
      GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
      g_autofree char *time = when (r), *score = score_text (r), *sub = row_sub (self, r);
      g_autofree char *title = g_strdup_printf ("%s · %s %s", time, named (self, "engine", str (r, "engine")), score);
      GtkWidget *row = neo_pressable_new (box);

      gtk_box_append (GTK_BOX (box), label_new (title, "nv-row-title"));
      gtk_box_append (GTK_BOX (box), label_new (sub, "nv-sub"));
      if (*str (r, "heard"))
        {
          g_autofree char *heard = g_strdup_printf ("«%s»", str (r, "heard"));

          gtk_box_append (GTK_BOX (box), label_new (heard, "nv-heard"));
        }
      gtk_widget_add_css_class (row, "nv-row");
      gtk_widget_add_css_class (row, g_strcmp0 (str (r, "outcome"), "accepted") ? "nv-rejected" : "nv-accepted");
      if (!g_strcmp0 (str (r, "id"), self->sel_id))
        gtk_widget_add_css_class (row, "nv-selected");
      neo_pressable_set_ripple (NEO_PRESSABLE (row), NEO_RIPPLE_BOUNDED, 0);
      g_object_set_data (G_OBJECT (row), "nv-record", r);
      g_signal_connect (row, "clicked", G_CALLBACK (on_row), self);
      gtk_box_append (GTK_BOX (self->rows), row);
    }
}

static void
on_list (gpointer data, JsonNode *reply, const char *error)
{
  Win *self = data;
  JsonArray *array = NULL;

  if (error)
    {
      gtk_label_set_text (GTK_LABEL (self->empty), error);
      gtk_widget_set_visible (self->empty, TRUE);
      return;
    }
  g_clear_pointer (&self->records, json_node_unref);
  self->records = json_node_ref (reply);
  fill_rows (self);
  array = JSON_NODE_HOLDS_ARRAY (reply) ? json_node_get_array (reply) : NULL;
  for (guint i = 0; array && i < json_array_get_length (array); i++)   /* the selection stays across a reload */
    {
      JsonObject *r = json_array_get_object_element (array, i);
      const char *want = self->want_id ? self->want_id : self->sel_id;

      if (want && !g_strcmp0 (str (r, "id"), want))
        {
          gboolean opened = self->detail_open || self->want_id;

          g_clear_pointer (&self->want_id, g_free);
          show_detail (self, r);
          self->detail_open = opened;
          update_chrome (self);
          return;
        }
    }
}

static void
on_status (gpointer data, JsonNode *reply, const char *error)
{
  Win *self = data;
  JsonObject *s = reply && JSON_NODE_HOLDS_OBJECT (reply) ? json_node_get_object (reply) : NULL;
  double rate = 0, fal = -1, pos = 0, neg = 0;
  g_autofree char *fal_text = NULL, *text = NULL;

  if (!s)
    {
      gtk_widget_set_visible (self->status, FALSE);
      return;
    }
  num (s, "triggers_per_h", &rate);
  num (s, "owner_pos_since_retrain", &pos);
  num (s, "owner_neg_since_retrain", &neg);
  fal_text = num (s, "false_trigger_rate", &fal) ? g_strdup_printf ("%.0f%%", fal * 100)
                                                 : g_strdup (nv_ui (self->store, "status_none"));
  text = g_strdup_printf (nv_ui (self->store, "status"), rate, fal_text, (int) pos, (int) neg);
  gtk_label_set_text (GTK_LABEL (self->status), text);
  gtk_widget_set_visible (self->status, TRUE);
}

static char *
filter_json (Win *self)
{
  g_autoptr(JsonBuilder) b = json_builder_new ();
  g_autoptr(JsonGenerator) gen = json_generator_new ();
  g_autoptr(JsonNode) root = NULL;

  json_builder_begin_object (b);
  if (!g_strcmp0 (self->filter, "unlabelled"))
    {
      json_builder_set_member_name (b, "label");
      json_builder_add_string_value (b, "unlabelled");
    }
  else if (!g_strcmp0 (self->filter, "accepted") || !g_strcmp0 (self->filter, "rejected"))
    {
      json_builder_set_member_name (b, "outcome");
      json_builder_add_string_value (b, self->filter);
    }
  json_builder_set_member_name (b, "limit");
  json_builder_add_int_value (b, nv_conf_int (self->store, "list_limit"));
  json_builder_end_object (b);
  root = json_builder_get_root (b);
  json_generator_set_root (gen, root);
  return json_generator_to_data (gen, NULL);
}

static void
load (Win *self)
{
  g_autofree char *filter = filter_json (self);

  nv_list (self->store, filter, on_list, self);
  nv_status (self->store, on_status, self);
}

static void
on_filter (GtkWidget *button, gpointer data)
{
  Win *self = data;

  g_free (self->filter);
  self->filter = g_strdup (g_object_get_data (G_OBJECT (button), "nv-id"));
  for (GtkWidget *b = gtk_widget_get_first_child (self->filters); b; b = gtk_widget_get_next_sibling (b))
    {
      if (b == button)
        gtk_widget_add_css_class (b, "nv-on");
      else
        gtk_widget_remove_css_class (b, "nv-on");
    }
  load (self);
}

/* ---- the detail ---- */

static void
on_playing (GObject *media, GParamSpec *pspec, gpointer data)
{
  Win *self = data;
  gboolean playing = gtk_media_stream_get_playing (GTK_MEDIA_STREAM (media));

  neo_pressable_set_child (NEO_PRESSABLE (self->play_btn),
                           label_new (nv_ui (self->store, playing ? "detail.stop" : "detail.play"), "nv-button"));
  if (!playing)
    nv_wave_set_playhead (self->wave, -1);
}

static void
on_timestamp (GObject *media, GParamSpec *pspec, gpointer data)
{
  Win *self = data;

  if (gtk_media_stream_get_playing (GTK_MEDIA_STREAM (media)))
    nv_wave_set_playhead (self->wave, (int) (gtk_media_stream_get_timestamp (GTK_MEDIA_STREAM (media)) / 1000));
}

static void
on_play (GtkWidget *button, gpointer data)
{
  Win *self = data;

  if (!self->clip)
    return;
  if (self->media && gtk_media_stream_get_playing (self->media))
    {
      gtk_media_stream_pause (self->media);
      return;
    }
  if (!self->media)
    {
      g_autoptr(GInputStream) in = g_memory_input_stream_new_from_bytes (self->clip);

      self->media = gtk_media_file_new_for_input_stream (in);
      g_signal_connect (self->media, "notify::playing", G_CALLBACK (on_playing), self);
      g_signal_connect (self->media, "notify::timestamp", G_CALLBACK (on_timestamp), self);
    }
  gtk_media_stream_seek (self->media, 0);
  gtk_media_stream_play (self->media);
}

static void
on_clip (gpointer data, GBytes *wav, const char *error)
{
  Win *self = data;
  double a = 0, b = 0, wa = 0, wb = 0;
  JsonArray *trim = self->sel && json_object_has_member (self->sel, "trim_ms")
                    ? json_object_get_array_member (self->sel, "trim_ms") : NULL;
  JsonArray *word = self->sel && json_object_has_member (self->sel, "word_ms")
                    ? json_object_get_array_member (self->sel, "word_ms") : NULL;

  if (error)
    snack_error (self, error);
  g_clear_pointer (&self->clip, g_bytes_unref);
  self->clip = wav ? g_bytes_ref (wav) : NULL;
  if (trim && json_array_get_length (trim) == 2)
    a = json_array_get_int_element (trim, 0), b = json_array_get_int_element (trim, 1);
  if (word && json_array_get_length (word) == 2)
    wa = json_array_get_int_element (word, 0), wb = json_array_get_int_element (word, 1);
  nv_wave_set (self->wave, wav, (int) a, (int) b, (int) wa, (int) wb);
  gtk_widget_set_sensitive (self->play_btn, wav != NULL);
}

static void
detail_line (Win *self, const char *text)
{
  gtk_box_append (GTK_BOX (self->d_lines), label_new (text, "nv-sub"));
}

static void
mark_label (Win *self)
{
  for (GtkWidget *b = gtk_widget_get_first_child (self->labels); b; b = gtk_widget_get_next_sibling (b))
    {
      if (!g_strcmp0 (g_object_get_data (G_OBJECT (b), "nv-id"), str (self->sel, "label")))
        gtk_widget_add_css_class (b, "nv-on");
      else
        gtk_widget_remove_css_class (b, "nv-on");
    }
}

static void
show_detail (Win *self, JsonObject *record)
{
  GtkWidget *child = NULL;
  g_autofree char *time = NULL, *score = NULL, *line = NULL;
  double level = 0;
  gboolean audio = FALSE;

  if (!record)
    return;
  if (self->media)
    gtk_media_stream_pause (self->media);
  g_clear_object (&self->media);
  g_clear_pointer (&self->sel_node, json_node_unref);
  self->sel_node = json_node_init_object (json_node_alloc (), record);   /* a reload replaces the list */
  self->sel = json_node_get_object (self->sel_node);
  g_free (self->sel_id);
  self->sel_id = g_strdup (str (record, "id"));
  self->detail_open = TRUE;

  time = when (record);
  score = score_text (record);
  gtk_label_set_text (GTK_LABEL (self->d_title), time);
  while ((child = gtk_widget_get_first_child (self->d_lines)))
    gtk_box_remove (GTK_BOX (self->d_lines), child);
  line = g_strdup_printf ("%s · %s%s%s", named (self, "outcome", str (record, "outcome")),
                          named (self, "engine", str (record, "engine")), *score ? " " : "", score);
  detail_line (self, line);
  if (*str (record, "reason"))
    detail_line (self, named (self, "reason", str (record, "reason")));
  detail_line (self, named (self, "band", str (record, "speaker_band")));
  if (num (record, "energy_dbfs", &level))
    {
      g_autofree char *text = g_strdup_printf (nv_ui (self->store, "detail.level"), level);

      detail_line (self, text);
    }
  if (*str (record, "turn"))
    {
      g_autofree char *text = g_strdup_printf (nv_ui (self->store, "detail.turn"),
                                               named (self, "reason", str (record, "turn")));

      detail_line (self, text);
    }
  if (*str (record, "heard"))
    {
      g_autofree char *text = g_strdup_printf (nv_ui (self->store, "detail.heard"), str (record, "heard"));

      gtk_label_set_text (GTK_LABEL (self->d_heard), text);
    }
  gtk_widget_set_visible (self->d_heard, *str (record, "heard") != '\0');
  gtk_editable_set_text (GTK_EDITABLE (self->note), str (record, "note"));
  mark_label (self);

  audio = json_object_has_member (record, "audio") && json_object_get_boolean_member (record, "audio");
  gtk_widget_set_visible (self->wave, audio);
  gtk_widget_set_visible (self->play_btn, audio);
  gtk_widget_set_visible (self->trim_save, audio);
  gtk_widget_set_visible (self->trim_reset, audio);
  gtk_widget_set_visible (self->no_audio, !audio);
  gtk_widget_set_visible (self->pick_hint, FALSE);
  gtk_widget_set_visible (self->detail_box, TRUE);
  g_clear_pointer (&self->clip, g_bytes_unref);
  nv_wave_set (self->wave, NULL, 0, 0, 0, 0);
  if (audio)
    nv_clip (self->store, self->sel_id, on_clip, self);
  fill_rows (self);   /* the selected row's mark */
  update_chrome (self);
}

/* a change's reply is the record as it is now */
static void
on_changed (Win *self, JsonNode *reply, const char *error, const char *done)
{
  if (error)
    {
      snack_error (self, error);
      return;
    }
  snack (self, nv_ui (self->store, done));
  load (self);
}

static void on_labelled (gpointer d, JsonNode *r, const char *e) { on_changed (d, r, e, "snack.labelled"); }
static void on_noted (gpointer d, JsonNode *r, const char *e) { on_changed (d, r, e, "snack.noted"); }
static void on_trimmed (gpointer d, JsonNode *r, const char *e) { on_changed (d, r, e, "snack.trimmed"); }

static void
on_label (GtkWidget *button, gpointer data)
{
  Win *self = data;
  const char *label = g_object_get_data (G_OBJECT (button), "nv-id");

  if (!self->sel_id)
    return;
  /* the same label again takes it back: the record is unlabelled (undone by the next one) */
  if (!g_strcmp0 (label, str (self->sel, "label")))
    label = "unlabelled";
  nv_label (self->store, self->sel_id, label, gtk_editable_get_text (GTK_EDITABLE (self->note)), on_labelled, self);
}

static void
on_note (GtkWidget *button, gpointer data)
{
  Win *self = data;
  const char *label = str (self->sel, "label");

  if (self->sel_id)
    nv_label (self->store, self->sel_id, *label ? label : "unlabelled", gtk_editable_get_text (GTK_EDITABLE (self->note)),
              on_noted, self);
}

static void
on_trim_save (GtkWidget *button, gpointer data)
{
  Win *self = data;
  int a = 0, b = 0;

  nv_wave_get_trim (self->wave, &a, &b);
  if (self->sel_id)
    nv_trim (self->store, self->sel_id, a, b, on_trimmed, self);
}

static void
on_trim_reset (GtkWidget *button, gpointer data)
{
  Win *self = data;

  nv_wave_reset_trim (self->wave);
  if (self->sel_id)
    nv_trim (self->store, self->sel_id, 0, nv_wave_length_ms (self->wave), on_trimmed, self);
}

static void
on_deleted (gpointer data, JsonNode *reply, const char *error)
{
  Win *self = data;

  if (error)
    {
      snack_error (self, error);
      return;
    }
  g_clear_pointer (&self->sel_id, g_free);
  self->sel = NULL;
  self->detail_open = FALSE;
  gtk_widget_set_visible (self->detail_box, FALSE);
  gtk_widget_set_visible (self->pick_hint, TRUE);
  update_chrome (self);
  snack (self, nv_ui (self->store, "snack.deleted"));
  load (self);
}

static void
on_delete_response (NeoDialog *dialog, int response, gpointer data)
{
  Win *self = data;

  if (response == GTK_RESPONSE_ACCEPT && self->sel_id)
    nv_delete (self->store, self->sel_id, on_deleted, self);
}

static void
on_delete (GtkWidget *button, gpointer data)
{
  Win *self = data;
  NeoDialog *dialog = neo_dialog_new (nv_ui (self->store, "confirm.delete_title"));

  neo_dialog_set_message (dialog, nv_ui (self->store, "confirm.delete_msg"));
  neo_dialog_add_button (dialog, nv_ui (self->store, "confirm.cancel"), GTK_RESPONSE_CANCEL);
  neo_dialog_add_button (dialog, nv_ui (self->store, "confirm.delete"), GTK_RESPONSE_ACCEPT);
  g_signal_connect (dialog, "response", G_CALLBACK (on_delete_response), self);
  neo_dialog_present (dialog, GTK_WIDGET (self->win));
}

static GtkWidget *
text_button (const char *text, GCallback cb, gpointer data)
{
  GtkWidget *button = neo_text_button_new (text);

  g_signal_connect (button, "clicked", cb, data);
  return button;
}

static GtkWidget *
detail_page (Win *self)
{
  GtkWidget *scroller = gtk_scrolled_window_new ();
  GtkWidget *outer = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  GtkWidget *row = NULL, *note_row = NULL;
  g_autoptr(GPtrArray) order = nv_ui_list (self->store, "labels_order");

  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_widget_set_hexpand (scroller, TRUE);
  gtk_widget_set_vexpand (scroller, TRUE);
  gtk_widget_add_css_class (box, "nv-detail");
  self->pick_hint = label_new (nv_ui (self->store, "app.choose"), "nv-sub");
  gtk_widget_set_valign (self->pick_hint, GTK_ALIGN_CENTER);
  gtk_widget_set_halign (self->pick_hint, GTK_ALIGN_CENTER);
  gtk_widget_set_vexpand (self->pick_hint, TRUE);
  self->detail_box = box;
  gtk_widget_set_visible (box, FALSE);

  self->d_title = label_new ("", "nv-title");
  self->d_lines = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
  self->d_heard = label_new ("", "nv-heard");
  self->wave = nv_wave_new ((int) nv_conf_int (self->store, "wave_height"),
                            (int) nv_conf_int (self->store, "handle_grab_px"));
  self->no_audio = label_new (nv_ui (self->store, "detail.no_audio"), "nv-sub");
  row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
  self->play_btn = text_button (nv_ui (self->store, "detail.play"), G_CALLBACK (on_play), self);
  self->trim_save = text_button (nv_ui (self->store, "detail.trim_save"), G_CALLBACK (on_trim_save), self);
  self->trim_reset = text_button (nv_ui (self->store, "detail.trim_reset"), G_CALLBACK (on_trim_reset), self);
  gtk_box_append (GTK_BOX (row), self->play_btn);
  gtk_box_append (GTK_BOX (row), self->trim_save);
  gtk_box_append (GTK_BOX (row), self->trim_reset);

  self->labels = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_add_css_class (self->labels, "nv-labels");
  for (guint i = 0; i < order->len; i++)
    {
      g_autofree char *key = g_strdup_printf ("label.%s", (char *) order->pdata[i]);
      GtkWidget *b = text_button (nv_ui (self->store, key), G_CALLBACK (on_label), self);

      g_object_set_data_full (G_OBJECT (b), "nv-id", g_strdup (order->pdata[i]), g_free);
      gtk_widget_add_css_class (b, "nv-chip");
      gtk_box_append (GTK_BOX (self->labels), b);
    }
  note_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  self->note = gtk_entry_new ();
  gtk_entry_set_placeholder_text (GTK_ENTRY (self->note), nv_ui (self->store, "detail.note"));
  gtk_widget_set_hexpand (self->note, TRUE);
  gtk_box_append (GTK_BOX (note_row), self->note);
  gtk_box_append (GTK_BOX (note_row), text_button (nv_ui (self->store, "detail.note_save"), G_CALLBACK (on_note), self));

  gtk_box_append (GTK_BOX (box), self->d_title);
  gtk_box_append (GTK_BOX (box), self->d_lines);
  gtk_box_append (GTK_BOX (box), self->d_heard);
  gtk_box_append (GTK_BOX (box), self->wave);
  gtk_box_append (GTK_BOX (box), self->no_audio);
  gtk_box_append (GTK_BOX (box), row);
  gtk_box_append (GTK_BOX (box), self->labels);
  gtk_box_append (GTK_BOX (box), note_row);
  gtk_box_append (GTK_BOX (box), text_button (nv_ui (self->store, "detail.delete"), G_CALLBACK (on_delete), self));
  gtk_box_append (GTK_BOX (outer), self->pick_hint);
  gtk_box_append (GTK_BOX (outer), box);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroller), outer);
  return scroller;
}

static GtkWidget *
list_page (Win *self)
{
  GtkWidget *page = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *scroller = gtk_scrolled_window_new ();
  GtkWidget *inner = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  g_autoptr(GPtrArray) order = nv_ui_list (self->store, "filters_order");

  self->status = label_new ("", "nv-status");
  gtk_widget_set_visible (self->status, FALSE);
  self->filters = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
  gtk_widget_add_css_class (self->filters, "nv-filters");
  for (guint i = 0; i < order->len; i++)
    {
      g_autofree char *key = g_strdup_printf ("filters.%s", (char *) order->pdata[i]);
      GtkWidget *b = text_button (nv_ui (self->store, key), G_CALLBACK (on_filter), self);

      g_object_set_data_full (G_OBJECT (b), "nv-id", g_strdup (order->pdata[i]), g_free);
      gtk_widget_add_css_class (b, "nv-chip");
      if (i == 0)
        {
          gtk_widget_add_css_class (b, "nv-on");
          self->filter = g_strdup (order->pdata[i]);
        }
      gtk_box_append (GTK_BOX (self->filters), b);
    }
  self->rows = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class (self->rows, "nv-card");
  self->empty = label_new (nv_ui (self->store, "app.empty"), "nv-sub");
  gtk_widget_add_css_class (self->empty, "nv-empty");
  gtk_widget_set_visible (self->empty, FALSE);
  gtk_box_append (GTK_BOX (inner), self->status);
  gtk_box_append (GTK_BOX (inner), self->empty);
  gtk_box_append (GTK_BOX (inner), self->rows);
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroller), inner);
  gtk_widget_set_vexpand (scroller, TRUE);
  gtk_box_append (GTK_BOX (page), self->filters);
  gtk_box_append (GTK_BOX (page), scroller);
  return page;
}

static void
win_free (Win *self)
{
  g_clear_object (&self->media);
  g_clear_pointer (&self->clip, g_bytes_unref);
  g_clear_pointer (&self->records, json_node_unref);
  g_clear_pointer (&self->sel_node, json_node_unref);
  g_free (self->sel_id);
  g_free (self->want_id);
  g_free (self->filter);
  g_free (self);
}

void
nv_window_select (GtkWidget *window, const char *id)
{
  Win *self = g_object_get_data (G_OBJECT (window), "nv-win");

  g_free (self->want_id);
  self->want_id = g_strdup (id);
  load (self);
}

GtkWidget *
nv_window_new (GtkApplication *app, NvStore *store)
{
  Win *self = g_new0 (Win, 1);
  GtkWidget *window = neo_window_new (app), *bar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 2), *refresh = NULL;
  GtkEventController *keys = gtk_shortcut_controller_new ();
  GtkOverlay *overlay = NULL;

  self->store = store;
  self->win = GTK_WINDOW (window);
  gtk_widget_add_css_class (window, "nv");
  g_object_set_data_full (G_OBJECT (window), "nv-win", self, (GDestroyNotify) win_free);
  g_signal_connect (window, "realize", G_CALLBACK (on_realize), self);
  gtk_shortcut_controller_add_shortcut (
    GTK_SHORTCUT_CONTROLLER (keys),
    gtk_shortcut_new (gtk_alternative_trigger_new (gtk_shortcut_trigger_parse_string ("<Alt>Left"),
                                                   gtk_shortcut_trigger_parse_string ("Escape")),
                      gtk_callback_action_new (on_back_action, self, NULL)));
  gtk_widget_add_controller (window, keys);

  gtk_widget_add_css_class (bar, "neo-app-bar");
  self->back_btn = neo_icon_button_new ("go-previous-symbolic");
  /* named as the shell's Back gesture looks for it: GTK's own translation of "Back" */
  gtk_accessible_update_property (GTK_ACCESSIBLE (self->back_btn), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                  g_dgettext ("gtk40", "Back"), -1);
  g_signal_connect (self->back_btn, "clicked", G_CALLBACK (on_back), self);
  self->title = label_new (nv_ui (store, "app.name"), "neo-app-bar-title");
  gtk_widget_set_hexpand (self->title, TRUE);
  refresh = neo_icon_button_new ("view-refresh-symbolic");
  g_signal_connect_swapped (refresh, "clicked", G_CALLBACK (load), self);
  gtk_box_append (GTK_BOX (bar), self->back_btn);
  gtk_box_append (GTK_BOX (bar), self->title);
  gtk_box_append (GTK_BOX (bar), refresh);

  self->list_root = list_page (self);
  self->detail_root = detail_page (self);
  self->snackbar = neo_snackbar_new ();
  self->stack = gtk_stack_new ();
  gtk_widget_set_vexpand (self->stack, TRUE);
  self->side = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_size_request (self->side, (int) nv_conf_int (store, "list_width"), -1);
  gtk_widget_add_css_class (self->side, "nv-side");
  self->body = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_vexpand (self->body, TRUE);
  gtk_box_append (GTK_BOX (self->body), self->side);
  gtk_box_append (GTK_BOX (self->body), self->stack);
  self->box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_append (GTK_BOX (self->box), bar);
  gtk_box_append (GTK_BOX (self->box), self->body);
  neo_window_set_content (self->win, self->box);
  overlay = neo_window_get_overlay (self->box);
  if (overlay)
    gtk_overlay_add_overlay (overlay, self->snackbar);
  relayout (self, FALSE);
  load (self);
  return window;
}
