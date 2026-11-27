/* The voice enrollment dialog ("Registra la mia voce"): Minerva's own face in the middle (the Avatar page's live
 * preview, without its pose strip), what she makes of every attempt, and Annulla in every state. Every text, the face
 * and effect of each state and the commands come from the data (minerva-config status: the voiceprint's "dialog",
 * Minerva's config/minerva_config/settings_ui.json). The enrollment itself runs in minerva-voice, started and
 * cancelled through minerva-config (its guarded methods take only listed callers); the dialog only listens to the
 * service's Enrollment events and MicLevel while it is open. Closing the dialog cancels a running enrollment. */
#include "ns-voice-enroll.h"
#include "ns-avatar-preview.h"
#include "ns-mc.h"
#include "msg-assistant.h"

#include <adwaita.h>

/* minerva-voice's and the kiosk's D-Bus API: the protocols' names */
#define VOICE_NAME  "org.minerva.Voice1"
#define VOICE_PATH  "/org/minerva/Voice1"
#define VOICE_IFACE "org.minerva.Voice1"
#define KIOSK_NAME  "org.minerva.Kiosk1"
#define KIOSK_PATH  "/org/minerva/Kiosk1"
#define KIOSK_IFACE "org.minerva.Kiosk1"

typedef struct {
  AdwDialog         *dialog;
  GtkWidget         *face_slot, *face, *title, *hint, *dots, *cancel, *action;
  JsonObject        *conf;
  GDBusConnection   *bus;
  guint              sub_event, sub_level, back_id;
  GCancellable      *cancel_io;
  char              *state, *reason;
  int                got, need;
  gboolean           running, closed;
  int                refs;      /* the open dialog's, and one per start command in flight */
  NsVoiceEnrollDone  done;
  gpointer           data;
} Ve;

static Ve *
ve_ref (Ve *v)
{
  v->refs++;
  return v;
}

static void
ve_unref (Ve *v)
{
  if (--v->refs > 0)
    return;
  g_clear_pointer (&v->conf, json_object_unref);
  g_free (v->state);
  g_free (v->reason);
  g_free (v);
}

static JsonObject *
state_conf (Ve *v, const char *state)
{
  return ns_json_obj (ns_json_obj (v->conf, "states"), state);
}

static char *
fill (Ve *v, const char *text)
{
  g_autoptr (GString) t = g_string_new (text ? text : "");
  g_autofree char *got = g_strdup_printf ("%d", v->got), *need = g_strdup_printf ("%d", v->need);
  g_autofree char *left = g_strdup_printf ("%d", MAX (0, v->need - v->got));
  g_string_replace (t, "{got}", got, 0);
  g_string_replace (t, "{need}", need, 0);
  g_string_replace (t, "{left}", left, 0);
  g_string_replace (t, "{reason}", v->reason ? v->reason : "", 0);
  return msg_assistant_fill (t->str);
}

static void
show_dots (Ve *v)
{
  g_autoptr (GString) t = g_string_new (NULL);
  for (int i = 0; i < v->need; i++)
    g_string_append (t, i < v->got ? "● " : "○ ");
  gtk_label_set_text (GTK_LABEL (v->dots), t->str);
  gtk_widget_set_visible (v->dots, v->need > 0);
}

static gboolean back_to_listening (gpointer data);

/* the state's texts, face and buttons */
static void
set_state (Ve *v, const char *state)
{
  JsonObject *s = state_conf (v, state);
  g_autofree char *title = fill (v, ns_json_str (s, "title"));
  g_autofree char *hint = fill (v, ns_json_str (s, "hint"));

  g_free (v->state);
  v->state = g_strdup (state);
  if (v->back_id) {
    g_source_remove (v->back_id);
    v->back_id = 0;
  }
  gtk_label_set_text (GTK_LABEL (v->title), title);
  gtk_label_set_text (GTK_LABEL (v->hint), hint);
  show_dots (v);
  ns_avatar_preview_set_pose (v->face, ns_json_str (s, "pose"), ns_json_str (s, "effect"));

  /* the one action: Inizia before, Fine after a voiceprint, Riprova after a failure or the timeout; none meanwhile */
  const char *action = !g_strcmp0 (state, "ready") ? "start" : !g_strcmp0 (state, "done") ? "done"
                       : (!g_strcmp0 (state, "expired") || !g_strcmp0 (state, "failed")) ? "retry" : NULL;
  gtk_widget_set_visible (v->action, action != NULL);
  gtk_widget_set_sensitive (v->action, TRUE);
  if (action)
    gtk_button_set_label (GTK_BUTTON (v->action), ns_json_str (v->conf, action));
  g_object_set_data (G_OBJECT (v->action), "ve-action", (gpointer) action);
  gtk_widget_set_visible (v->cancel, g_strcmp0 (state, "done") != 0);

  /* a sample's answer stays for its show_ms, then she listens again */
  JsonObject *show = ns_json_obj (v->conf, "show_ms");
  if (show && json_object_has_member (show, state) && v->running)
    v->back_id = g_timeout_add ((guint) json_object_get_int_member (show, state), back_to_listening, v);
}

static gboolean
back_to_listening (gpointer data)
{
  Ve *v = data;
  v->back_id = 0;
  if (v->running)
    set_state (v, "listening");
  return G_SOURCE_REMOVE;
}

static GStrv
args_of (Ve *v, const char *member)
{
  JsonArray *a = ns_json_arr (v->conf, member);
  g_autoptr (GStrvBuilder) b = g_strv_builder_new ();
  for (guint i = 0; a && i < json_array_get_length (a); i++)
    g_strv_builder_add (b, json_array_get_string_element (a, i));
  return g_strv_builder_end (b);
}

static void
started (JsonObject *root, const char *err, gpointer data)
{
  Ve *v = data;
  if (v->closed) {
    ve_unref (v);
    return;
  }
  if (err) {
    g_free (v->reason);
    v->reason = g_strdup (err);
    set_state (v, "failed");
    ve_unref (v);
    return;
  }
  v->running = TRUE;
  if (!g_strcmp0 (v->state, "ready") || !g_strcmp0 (v->state, "failed") || !g_strcmp0 (v->state, "expired"))
    set_state (v, "listening");
  ve_unref (v);
}

static void
start (Ve *v)
{
  g_auto (GStrv) args = args_of (v, "start_args");
  v->got = 0;
  gtk_widget_set_sensitive (v->action, FALSE);
  ns_mc_run (G_OBJECT (v->dialog), (const char *const *) args, NULL, started, ve_ref (v));
}

static void
on_action (GtkButton *b, Ve *v)
{
  const char *action = g_object_get_data (G_OBJECT (b), "ve-action");
  if (!g_strcmp0 (action, "done"))
    adw_dialog_close (v->dialog);
  else if (action)
    start (v);
}

/* minerva-voice's Enrollment event: {kind, got, need, reason, text} */
static void
on_event (GDBusConnection *c, const char *sender, const char *path, const char *iface, const char *signal,
          GVariant *params, gpointer data)
{
  Ve *v = data;
  const char *json = NULL;
  g_autoptr (JsonParser) parser = json_parser_new ();

  if (v->closed || !g_variant_is_of_type (params, G_VARIANT_TYPE ("(s)")))
    return;
  g_variant_get (params, "(&s)", &json);
  if (!json_parser_load_from_data (parser, json, -1, NULL) || !JSON_NODE_HOLDS_OBJECT (json_parser_get_root (parser)))
    return;
  JsonObject *e = json_node_get_object (json_parser_get_root (parser));
  const char *kind = ns_json_str (e, "kind");
  v->got = (int) json_object_get_int_member_with_default (e, "got", v->got);
  v->need = (int) json_object_get_int_member_with_default (e, "need", v->need);
  g_free (v->reason);
  v->reason = g_strdup (ns_json_str (e, "text"));

  if (!g_strcmp0 (kind, "started")) {
    v->running = TRUE;
    set_state (v, "listening");
  } else if (!g_strcmp0 (kind, "accepted")) {
    set_state (v, "accepted");
  } else if (!g_strcmp0 (kind, "rejected")) {
    set_state (v, "rejected");
  } else if (!g_strcmp0 (kind, "done")) {
    v->running = FALSE;
    set_state (v, "done");
  } else if (!g_strcmp0 (kind, "expired")) {
    v->running = FALSE;
    set_state (v, "expired");
  } else if (!g_strcmp0 (kind, "cancelled")) {
    v->running = FALSE;   /* cancelled from elsewhere (or by our own close): nothing left to show */
    adw_dialog_close (v->dialog);
  }
}

/* the microphone's level while she listens: the face's wave */
static void
on_level (GDBusConnection *c, const char *sender, const char *path, const char *iface, const char *signal,
          GVariant *params, gpointer data)
{
  Ve *v = data;
  double level = 0;

  if (v->closed || !g_variant_is_of_type (params, G_VARIANT_TYPE ("(d)")) || g_strcmp0 (v->state, "listening"))
    return;
  g_variant_get (params, "(d)", &level);
  ns_avatar_preview_level (v->face, level);
}

/* the kiosk's preview data (its SettingsDescribe "preview"): the face in the middle, in the current state */
static void
described (GObject *src, GAsyncResult *res, gpointer data)
{
  g_autoptr (GVariant) out = g_dbus_connection_call_finish (G_DBUS_CONNECTION (src), res, NULL);
  g_autoptr (JsonParser) parser = json_parser_new ();
  const char *json = NULL;
  Ve *v = data;

  if (out == NULL)
    return;   /* no kiosk (or the dialog closed: cancelled): the dialog goes on without her face */
  g_variant_get (out, "(&s)", &json);
  if (!json_parser_load_from_data (parser, json, -1, NULL) || !JSON_NODE_HOLDS_OBJECT (json_parser_get_root (parser)))
    return;
  v->face = ns_avatar_preview_new_bare (ns_json_obj (json_node_get_object (json_parser_get_root (parser)), "preview"));
  if (v->face == NULL)
    return;
  gtk_box_append (GTK_BOX (v->face_slot), v->face);
  JsonObject *s = state_conf (v, v->state);
  ns_avatar_preview_set_pose (v->face, ns_json_str (s, "pose"), ns_json_str (s, "effect"));
}

static void
cancelled (JsonObject *root, const char *err, gpointer data)
{
  if (err)
    g_message ("voice enrollment not cancelled: %s", err);
}

static void
on_closed (Ve *v)
{
  v->closed = TRUE;
  if (v->running) {   /* closing is cancelling: the samples so far are dropped, a saved voiceprint stays */
    g_auto (GStrv) args = args_of (v, "cancel_args");
    ns_mc_run (G_OBJECT (v->dialog), (const char *const *) args, NULL, cancelled, NULL);
  }
  if (v->back_id)
    g_source_remove (v->back_id);
  g_cancellable_cancel (v->cancel_io);
  if (v->bus) {
    g_dbus_connection_signal_unsubscribe (v->bus, v->sub_event);
    g_dbus_connection_signal_unsubscribe (v->bus, v->sub_level);
  }
  if (v->done)
    v->done (v->data);
  g_clear_object (&v->cancel_io);
  g_clear_object (&v->bus);
  g_clear_object (&v->dialog);
  ve_unref (v);
}

static GtkWidget *
label (const char *css)
{
  GtkWidget *l = gtk_label_new (NULL);
  gtk_label_set_wrap (GTK_LABEL (l), TRUE);
  gtk_label_set_justify (GTK_LABEL (l), GTK_JUSTIFY_CENTER);
  if (css)
    gtk_widget_add_css_class (l, css);
  return l;
}

void
ns_voice_enroll_open (GtkWidget *parent, JsonObject *conf, NsVoiceEnrollDone done, gpointer data)
{
  Ve *v = g_new0 (Ve, 1);
  g_autofree char *title = NULL;

  v->refs = 1;
  v->conf = json_object_ref (conf);
  v->done = done;
  v->data = data;
  v->cancel_io = g_cancellable_new ();
  v->state = g_strdup ("ready");
  v->dialog = g_object_ref_sink (ADW_DIALOG (adw_dialog_new ()));
  title = msg_assistant_fill (ns_json_str (conf, "title"));
  adw_dialog_set_title (v->dialog, title);
  adw_dialog_set_presentation_mode (v->dialog, ADW_DIALOG_FLOATING);

  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_start (box, 18);
  gtk_widget_set_margin_end (box, 18);
  gtk_widget_set_margin_top (box, 6);
  gtk_widget_set_margin_bottom (box, 18);
  v->face_slot = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_append (GTK_BOX (box), v->face_slot);
  v->title = label ("title-2");
  v->hint = label (NULL);
  v->dots = label ("title-3");
  gtk_box_append (GTK_BOX (box), v->title);
  gtk_box_append (GTK_BOX (box), v->hint);
  gtk_box_append (GTK_BOX (box), v->dots);

  GtkWidget *buttons = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_box_set_homogeneous (GTK_BOX (buttons), TRUE);
  v->cancel = gtk_button_new_with_label (ns_json_str (conf, "cancel"));
  gtk_widget_add_css_class (v->cancel, "pill");
  g_signal_connect_swapped (v->cancel, "clicked", G_CALLBACK (adw_dialog_close), v->dialog);
  v->action = gtk_button_new ();
  gtk_widget_add_css_class (v->action, "pill");
  gtk_widget_add_css_class (v->action, "suggested-action");
  g_signal_connect (v->action, "clicked", G_CALLBACK (on_action), v);
  gtk_box_append (GTK_BOX (buttons), v->cancel);
  gtk_box_append (GTK_BOX (buttons), v->action);
  gtk_box_append (GTK_BOX (box), buttons);

  GtkWidget *toolbar = adw_toolbar_view_new ();
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (toolbar), adw_header_bar_new ());
  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (toolbar), box);
  adw_dialog_set_child (v->dialog, toolbar);
  g_signal_connect_swapped (v->dialog, "closed", G_CALLBACK (on_closed), v);

  v->bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, NULL);
  if (v->bus) {
    v->sub_event = g_dbus_connection_signal_subscribe (v->bus, VOICE_NAME, VOICE_IFACE, "Enrollment", VOICE_PATH, NULL,
                                                       G_DBUS_SIGNAL_FLAGS_NONE, on_event, v, NULL);
    v->sub_level = g_dbus_connection_signal_subscribe (v->bus, VOICE_NAME, VOICE_IFACE, "MicLevel", VOICE_PATH, NULL,
                                                       G_DBUS_SIGNAL_FLAGS_NONE, on_level, v, NULL);
    g_dbus_connection_call (v->bus, KIOSK_NAME, KIOSK_PATH, KIOSK_IFACE, "SettingsDescribe", NULL,
                            G_VARIANT_TYPE ("(s)"), G_DBUS_CALL_FLAGS_NO_AUTO_START, -1, v->cancel_io, described, v);
  }
  set_state (v, "ready");
  adw_dialog_present (v->dialog, parent);
}
