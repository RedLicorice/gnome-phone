/* Minerva > Persone: the people Minerva knows (Minerva's people/ store), each with a notebook, face samples, a
 * voiceprint and a linked contact. Every label comes from `minerva-config people ui` (data:
 * config/minerva_config/people_ui.json); everything is read and written by `minerva-config people … --json`, which
 * answers {"ok": false, "error": …} for what it refused: the page shows that message and changes nothing. */
#include "ns-pages.h"
#include "ns-mc.h"
#include "ns-ui.h"

#include <json-glib/json-glib.h>

#define str_of ns_json_str
#define obj_of ns_json_obj
#define arr_of ns_json_arr

static gboolean
ok_of (JsonObject *root)
{
  return root && json_object_get_boolean_member_with_default (root, "ok", FALSE);
}

static gint64
int_of (JsonObject *o, const char *member)
{
  return o ? json_object_get_int_member_with_default (o, member, 0) : 0;
}

static gboolean
bool_of (JsonObject *o, const char *member)
{
  return o ? json_object_get_boolean_member_with_default (o, member, FALSE) : FALSE;
}

/* a text of the data with "{key}" replaced, NULL ends the pairs */
static char *
fmt (const char *tpl, ...)
{
  GString *s = g_string_new (tpl ? tpl : "");
  va_list ap;
  va_start (ap, tpl);
  const char *key;
  while ((key = va_arg (ap, const char *))) {
    const char *val = va_arg (ap, const char *);
    g_autofree char *mark = g_strdup_printf ("{%s}", key);
    g_string_replace (s, mark, val ? val : "", 0);
  }
  va_end (ap);
  return g_string_free (s, FALSE);
}

static char *
fmt_n (const char *tpl, const char *key, gint64 n)
{
  g_autofree char *v = g_strdup_printf ("%" G_GINT64_FORMAT, n);
  return fmt (tpl, key, v, NULL);
}

static GtkWidget *
msg_new (GtkWidget *content)
{
  GtkWidget *l = ns_note (content, "");
  gtk_label_set_selectable (GTK_LABEL (l), TRUE);
  gtk_widget_set_visible (l, FALSE);
  return l;
}

static void
msg_set (GtkWidget *label, const char *text, gboolean error)
{
  gtk_label_set_text (GTK_LABEL (label), text ? text : "");
  gtk_widget_set_visible (label, text && *text);
  if (error)
    gtk_widget_add_css_class (label, "ns-danger");
  else
    gtk_widget_remove_css_class (label, "ns-danger");
}

/* the answer's problem: the program failed (err) or refused ({"ok": false}); NULL when it did it */
static const char *
problem (JsonObject *root, const char *err)
{
  if (err)
    return err;
  if (!root)
    return "?";
  return ok_of (root) ? NULL : str_of (root, "error");
}

/* ======================= one person ======================= */

typedef struct {
  GObject    *owner;            /* the page: kept alive by the running jobs */
  JsonObject *ui;
  char       *id, *name, *pending_uid, *pending_contact;
  gboolean    enrolled;
  GtkWidget  *msg, *nb_view, *faces_row, *voice_row, *enroll_btn, *forget_btn, *contact_row, *unlink_btn;
  guint       poll_id, poll_s;
} Pd;

static const char *
d_str (Pd *d, const char *key)
{
  return str_of (obj_of (d->ui, "detail"), key);
}

static void
pd_free (Pd *d)
{
  if (d->poll_id)
    g_source_remove (d->poll_id);
  json_object_unref (d->ui);
  g_free (d->id);
  g_free (d->name);
  g_free (d->pending_uid);
  g_free (d->pending_contact);
  g_free (d);
}

static GtkWidget *
editor_new (void)
{
  GtkWidget *view = gtk_text_view_new ();
  gtk_text_view_set_wrap_mode (GTK_TEXT_VIEW (view), GTK_WRAP_WORD_CHAR);
  gtk_widget_add_css_class (view, "ns-editor");
  GtkWidget *sw = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (sw), view);
  gtk_widget_add_css_class (sw, "ns-editor-box");
  g_object_set_data (G_OBJECT (sw), "pp-view", view);
  return sw;
}

static GtkTextBuffer *
buffer_of (GtkWidget *sw)
{
  return gtk_text_view_get_buffer (GTK_TEXT_VIEW (g_object_get_data (G_OBJECT (sw), "pp-view")));
}

static void voice_load (Pd *d);
static void person_load (Pd *d);

static gboolean
poll_tick (gpointer data)
{
  Pd *d = data;
  d->poll_id = 0;
  voice_load (d);
  return G_SOURCE_REMOVE;
}

static void
voice_loaded (JsonObject *root, const char *err, gpointer data)
{
  Pd *d = data;
  if (problem (root, err)) {
    msg_set (d->msg, problem (root, err), TRUE);
    return;
  }
  gboolean running = bool_of (root, "running");
  gboolean mine = bool_of (root, "enrolling") && !g_strcmp0 (str_of (root, "person"), d->id);
  gboolean busy = bool_of (root, "enrolling");
  JsonArray *voices = arr_of (root, "voices");
  for (guint i = 0; voices && i < json_array_get_length (voices); i++)
    if (!g_strcmp0 (json_array_get_string_element (voices, i), d->id))
      d->enrolled = TRUE;
  g_autofree char *sub = NULL;
  if (!running)
    sub = g_strdup (d_str (d, "voice_off"));
  else if (mine) {
    g_autofree char *got = g_strdup_printf ("%" G_GINT64_FORMAT, int_of (root, "got"));
    g_autofree char *need = g_strdup_printf ("%" G_GINT64_FORMAT, int_of (root, "need"));
    sub = fmt (d_str (d, "voice_enrolling"), "got", got, "need", need, "name", d->name, NULL);
  } else
    sub = g_strdup (d_str (d, d->enrolled ? "voice_enrolled" : "voice_none"));
  ns_row_set_subtitle (d->voice_row, sub);
  gtk_widget_set_visible (d->enroll_btn, running && !busy);
  gtk_widget_set_visible (d->forget_btn, running && !mine);
  gtk_widget_set_sensitive (d->forget_btn, d->enrolled);
  if (busy && !d->poll_id)
    d->poll_id = g_timeout_add_seconds (d->poll_s, poll_tick, d);
}

static void
voice_load (Pd *d)
{
  static const char *const args[] = { "people", "voice-status", "--json", NULL };
  ns_mc_run (d->owner, args, NULL, voice_loaded, d);
}

static void
person_loaded (JsonObject *root, const char *err, gpointer data)
{
  Pd *d = data;
  if (problem (root, err)) {
    msg_set (d->msg, problem (root, err), TRUE);
    return;
  }
  g_free (d->name);
  d->name = g_strdup (str_of (root, "name"));
  adw_navigation_page_set_title (ADW_NAVIGATION_PAGE (d->owner), d->name);
  gint64 faces = int_of (root, "faces");
  g_autofree char *fv = faces ? fmt_n (d_str (d, "faces_value"), "n", faces) : g_strdup (d_str (d, "faces_none"));
  ns_row_set_subtitle (d->faces_row, fv);
  d->enrolled = bool_of (root, "voiceprint");
  gboolean linked = *str_of (root, "contact_uid") != '\0';
  const char *cname = *str_of (root, "contact_name") ? str_of (root, "contact_name") : str_of (root, "contact_uid");
  ns_row_set_subtitle (d->contact_row, linked ? cname : d_str (d, "contact_none"));
  gtk_widget_set_visible (d->unlink_btn, linked);
  voice_load (d);
}

static void
notebook_loaded (JsonObject *root, const char *err, gpointer data)
{
  Pd *d = data;
  if (problem (root, err)) {
    msg_set (d->msg, problem (root, err), TRUE);
    return;
  }
  gtk_text_buffer_set_text (buffer_of (d->nb_view), str_of (root, "text"), -1);
}

static void
person_load (Pd *d)
{
  const char *const show[] = { "people", "show", d->id, "--json", NULL };
  ns_mc_run (d->owner, show, NULL, person_loaded, d);
}

static void
notebook_load (Pd *d)
{
  const char *const get[] = { "people", "notebook", d->id, "get", "--json", NULL };
  ns_mc_run (d->owner, get, NULL, notebook_loaded, d);
}

/* an action's answer: the message of the data (`done` key of "detail") or its problem, then the page again */
typedef struct { Pd *d; const char *done; gboolean pop; } Ans;

static void
answered (JsonObject *root, const char *err, gpointer data)
{
  Ans *a = data;
  Pd *d = a->d;
  const char *why = problem (root, err);
  if (why)
    msg_set (d->msg, why, TRUE);
  else if (a->pop)
    gtk_widget_activate_action (d->msg, "navigation.pop", NULL);
  else {
    msg_set (d->msg, a->done ? d_str (d, a->done) : "", FALSE);
    person_load (d);
    notebook_load (d);
  }
  g_free (a);
}

static void
run (Pd *d, const char *const *args, const char *stdin_text, const char *done, gboolean pop)
{
  Ans *a = g_new0 (Ans, 1);
  a->d = d;
  a->done = done;
  a->pop = pop;
  ns_mc_run (d->owner, args, stdin_text, answered, a);
}

static void
save_tapped (GtkWidget *row, gpointer data)
{
  Pd *d = data;
  GtkTextIter s, e;
  gtk_text_buffer_get_bounds (buffer_of (d->nb_view), &s, &e);
  g_autofree char *text = gtk_text_buffer_get_text (buffer_of (d->nb_view), &s, &e, FALSE);
  const char *const args[] = { "people", "notebook", d->id, "set", "--stdin", "--json", NULL };
  run (d, args, text, "saved", FALSE);
}

static void
clear_confirmed (gboolean yes, gpointer data)
{
  Pd *d = data;
  const char *const args[] = { "people", "notebook", d->id, "clear", "--json", NULL };
  if (yes)
    run (d, args, NULL, "cleared", FALSE);
}

static void
clear_tapped (GtkWidget *row, gpointer data)
{
  Pd *d = data;
  g_autofree char *body = fmt (d_str (d, "clear_body"), "name", d->name, NULL);
  ns_confirm (row, d_str (d, "clear_heading"), body, d_str (d, "clear"), TRUE, clear_confirmed, d);
}

static void
consent_given (gboolean yes, gpointer data)
{
  Pd *d = data;
  if (!yes)
    return;
  /* the consent is the confirmation the user read and accepted, word for word */
  g_autofree char *consent = fmt (d_str (d, "consent_body"), "name", d->name, NULL);
  const char *const args[] = { "people", "voice-enroll", d->id, "--consent", consent, "--json", NULL };
  run (d, args, NULL, "voice_started", FALSE);
}

static void
enroll_tapped (GtkWidget *row, gpointer data)
{
  Pd *d = data;
  g_autofree char *body = fmt (d_str (d, "consent_body"), "name", d->name, NULL);
  ns_confirm (row, d_str (d, "consent_heading"), body, d_str (d, "consent_ok"), FALSE, consent_given, d);
}

static void
forget_confirmed (gboolean yes, gpointer data)
{
  Pd *d = data;
  const char *const args[] = { "people", "voice-forget", d->id, "--json", NULL };
  if (yes) {
    d->enrolled = FALSE;
    run (d, args, NULL, "voice_forgotten", FALSE);
  }
}

static void
forget_tapped (GtkWidget *row, gpointer data)
{
  Pd *d = data;
  g_autofree char *body = fmt (d_str (d, "voice_forget_body"), "name", d->name, NULL);
  ns_confirm (row, d_str (d, "voice_forget_heading"), body, d_str (d, "voice_forget"), TRUE, forget_confirmed, d);
}

static void
unlink_tapped (GtkWidget *row, gpointer data)
{
  Pd *d = data;
  const char *const args[] = { "people", "unlink", d->id, "--json", NULL };
  run (d, args, NULL, NULL, FALSE);
}

static void
delete_confirmed (gboolean yes, gpointer data)
{
  Pd *d = data;
  const char *const args[] = { "people", "delete", d->id, "--json", NULL };
  if (yes)
    run (d, args, NULL, NULL, TRUE);
}

static void
delete_tapped (GtkWidget *row, gpointer data)
{
  Pd *d = data;
  g_autofree char *heading = fmt (d_str (d, "delete_heading"), "name", d->name, NULL);
  ns_confirm (row, heading, d_str (d, "delete_body"), d_str (d, "delete_ok"), TRUE, delete_confirmed, d);
}

/* ---- Collega: the address book's contacts with a similar name, a tap links one (asked first) ---- */

typedef struct { Pd *d; GObject *page; GtkWidget *group, *msg; } Pick;   /* the Collega page's */

static void
link_answered (JsonObject *root, const char *err, gpointer data)
{
  Pick *k = data;
  const char *why = problem (root, err);
  if (why)
    msg_set (k->msg, why, TRUE);
  else
    gtk_widget_activate_action (k->msg, "navigation.pop", NULL);   /* back to the person, which loads again */
}

static void
link_confirmed (gboolean yes, gpointer data)
{
  Pick *k = data;
  const char *const args[] = { "people", "link", k->d->id, k->d->pending_uid, "--json", NULL };
  if (yes)
    ns_mc_run (k->page, args, NULL, link_answered, k);   /* k lives with that page */
}

static void
contact_tapped (GtkWidget *row, gpointer data)
{
  Pick *k = data;
  Pd *d = k->d;
  g_free (d->pending_uid);
  g_free (d->pending_contact);
  d->pending_uid = g_strdup (g_object_get_data (G_OBJECT (row), "pp-uid"));
  d->pending_contact = g_strdup (g_object_get_data (G_OBJECT (row), "pp-name"));
  g_autofree char *heading = fmt (d_str (d, "link_heading"), "contact", d->pending_contact, NULL);
  g_autofree char *body = fmt (d_str (d, "link_body"), "contact", d->pending_contact, "name", d->name, NULL);
  ns_confirm (row, heading, body, d_str (d, "link_ok"), FALSE, link_confirmed, k);
}

static void
suggestions_loaded (JsonObject *root, const char *err, gpointer data)
{
  Pick *k = data;
  const char *why = problem (root, err);
  if (why) {
    msg_set (k->msg, why, TRUE);
    return;
  }
  ns_group_clear (k->group);
  JsonArray *list = arr_of (root, "suggestions");
  guint n = list ? json_array_get_length (list) : 0;
  if (!n)
    ns_row (k->group, NULL, NULL, str_of (obj_of (k->d->ui, "detail"), "suggest_empty"), NULL);
  for (guint i = 0; i < n; i++) {
    JsonObject *c = json_array_get_object_element (list, i);
    JsonArray *nums = arr_of (c, "numbers");
    g_autoptr (GString) sub = g_string_new ("");
    for (guint j = 0; nums && j < json_array_get_length (nums); j++)
      g_string_append_printf (sub, "%s%s", j ? " • " : "", json_array_get_string_element (nums, j));
    GtkWidget *row = ns_row (k->group, "avatar-default-symbolic", "green", str_of (c, "name"), sub->str);
    g_object_set_data_full (G_OBJECT (row), "pp-uid", g_strdup (str_of (c, "uid")), g_free);
    g_object_set_data_full (G_OBJECT (row), "pp-name", g_strdup (str_of (c, "name")), g_free);
    ns_row_on_activate (row, contact_tapped, k);
  }
}

static void
link_tapped (GtkWidget *row, gpointer data)
{
  Pd *d = data;
  GtkWidget *c;
  AdwNavigationPage *page = ns_page_new (d_str (d, "suggest_title"), &c);
  Pick *k = g_new0 (Pick, 1);
  k->d = d;   /* the person's page stays below this one while it is shown */
  k->page = G_OBJECT (page);
  g_object_set_data_full (G_OBJECT (page), "pp-pick", k, g_free);
  k->msg = msg_new (c);
  k->group = ns_group (c, NULL);
  ns_note (c, d_str (d, "suggest_note"));
  const char *const args[] = { "people", "suggest", d->id, "--json", NULL };
  ns_mc_run (G_OBJECT (page), args, NULL, suggestions_loaded, k);
  ns_push (page);
}

static void
person_showing (AdwNavigationPage *page, gpointer data)
{
  person_load (data);
}

static void
person_open (JsonObject *ui, const char *id, const char *name)
{
  GtkWidget *c;
  AdwNavigationPage *page = ns_page_new (name, &c);
  Pd *d = g_new0 (Pd, 1);
  d->owner = G_OBJECT (page);
  d->ui = json_object_ref (ui);
  d->id = g_strdup (id);
  d->name = g_strdup (name);
  gint64 poll = int_of (ui, "poll_s");
  d->poll_s = poll > 0 ? (guint) poll : 1;
  g_object_set_data_full (G_OBJECT (page), "pp", d, (GDestroyNotify) pd_free);

  d->msg = msg_new (c);

  g_autofree char *nb_label = fmt (d_str (d, "notebook_label"), "name", name, NULL);
  GtkWidget *g = ns_group (c, d_str (d, "notebook"));
  d->nb_view = editor_new ();
  ns_row_widget (g, nb_label, d->nb_view);
  ns_row_button (g, d_str (d, "save"), FALSE, save_tapped, d);
  ns_row_button (g, d_str (d, "clear"), TRUE, clear_tapped, d);
  ns_note (c, d_str (d, "notebook_note"));

  g = ns_group (c, d_str (d, "faces"));
  d->faces_row = ns_row (g, "camera-photo-symbolic", "orange", d_str (d, "faces_row"), "");
  ns_note (c, d_str (d, "faces_note"));

  g = ns_group (c, d_str (d, "voice"));
  d->voice_row = ns_row (g, "audio-input-microphone-symbolic", "violet", d_str (d, "voice_row"), "");
  d->enroll_btn = ns_row_button (g, d_str (d, "voice_enroll"), FALSE, enroll_tapped, d);
  d->forget_btn = ns_row_button (g, d_str (d, "voice_forget"), TRUE, forget_tapped, d);

  g = ns_group (c, d_str (d, "contact"));
  d->contact_row = ns_row (g, "contact-new-symbolic", "green", d_str (d, "contact_row"), "");
  ns_row_button (g, d_str (d, "link"), FALSE, link_tapped, d);
  d->unlink_btn = ns_row_button (g, d_str (d, "unlink"), TRUE, unlink_tapped, d);

  g = ns_group (c, NULL);
  ns_row_button (g, d_str (d, "delete"), TRUE, delete_tapped, d);

  /* each time the page shows (also back from Collega): the person again */
  g_signal_connect (page, "showing", G_CALLBACK (person_showing), d);
  notebook_load (d);
  ns_push (page);
}

/* ======================= the list ======================= */

typedef struct {
  GObject    *owner;
  JsonObject *ui;
  GtkWidget  *msg, *list_g;
} Pp;

static const char *
l_str (Pp *p, const char *key)
{
  return str_of (obj_of (p->ui, "list"), key);
}

static void
pp_free (Pp *p)
{
  json_object_unref (p->ui);
  g_free (p);
}

static void
person_tapped (GtkWidget *row, gpointer data)
{
  Pp *p = data;
  person_open (p->ui, g_object_get_data (G_OBJECT (row), "pp-id"), g_object_get_data (G_OBJECT (row), "pp-name"));
}

static void
list_loaded (JsonObject *root, const char *err, gpointer data)
{
  Pp *p = data;
  const char *why = problem (root, err);
  msg_set (p->msg, why, TRUE);
  if (why)
    return;
  ns_group_clear (p->list_g);
  JsonArray *people = arr_of (root, "people");
  guint n = people ? json_array_get_length (people) : 0;
  if (!n)
    ns_row (p->list_g, NULL, NULL, l_str (p, "empty"), NULL);
  for (guint i = 0; i < n; i++) {
    JsonObject *o = json_array_get_object_element (people, i);
    g_autofree char *faces = fmt_n (l_str (p, int_of (o, "faces") == 1 ? "faces_one" : "faces"), "n", int_of (o, "faces"));
    g_autofree char *notes = fmt_n (l_str (p, int_of (o, "notes") == 1 ? "notes_one" : "notes"), "n", int_of (o, "notes"));
    g_autofree char *sub = g_strdup_printf ("%s%s%s • %s • %s • %s", bool_of (o, "owner") ? l_str (p, "owner") : "",
                                            bool_of (o, "owner") ? " • " : "", faces,
                                            l_str (p, bool_of (o, "voiceprint") ? "voice_yes" : "voice_no"),
                                            l_str (p, *str_of (o, "contact_uid") ? "contact_yes" : "contact_no"), notes);
    GtkWidget *row = ns_row (p->list_g, "avatar-default-symbolic", "blue", str_of (o, "name"), sub);
    g_object_set_data_full (G_OBJECT (row), "pp-id", g_strdup (str_of (o, "id")), g_free);
    g_object_set_data_full (G_OBJECT (row), "pp-name", g_strdup (str_of (o, "name")), g_free);
    ns_row_on_activate (row, person_tapped, p);
  }
}

static void
list_load (Pp *p)
{
  static const char *const args[] = { "people", "list", "--json", NULL };
  ns_mc_run (p->owner, args, NULL, list_loaded, p);
}

static void
list_showing (AdwNavigationPage *page, gpointer data)
{
  list_load (data);
}

static void
created (JsonObject *root, const char *err, gpointer data)
{
  Pp *p = data;
  const char *why = problem (root, err);
  msg_set (p->msg, why, TRUE);
  if (why)
    return;
  list_load (p);
  person_open (p->ui, str_of (root, "id"), str_of (root, "name"));
}

static void
add_entered (const char *text, gpointer data)
{
  Pp *p = data;
  g_autofree char *name = g_strstrip (g_strdup (text ? text : ""));
  if (!*name)
    return;
  const char *const args[] = { "people", "create", name, "--json", NULL };
  ns_mc_run (p->owner, args, NULL, created, p);
}

static void
add_tapped (GtkWidget *row, gpointer data)
{
  Pp *p = data;
  ns_ask (row, l_str (p, "add_heading"), l_str (p, "add_body"), "", FALSE, l_str (p, "add_ok"), add_entered, p);
}

static void
ns_page_people (GtkWidget *row, gpointer data)
{
  JsonObject *ui = data;
  GtkWidget *c;
  AdwNavigationPage *page = ns_page_new (str_of (ui, "title"), &c);
  Pp *p = g_new0 (Pp, 1);
  p->owner = G_OBJECT (page);
  p->ui = json_object_ref (ui);
  g_object_set_data_full (G_OBJECT (page), "pp", p, (GDestroyNotify) pp_free);
  p->msg = msg_new (c);
  p->list_g = ns_group (c, l_str (p, "title"));
  ns_note (c, l_str (p, "note"));
  GtkWidget *g = ns_group (c, NULL);
  ns_row_button (g, l_str (p, "add"), FALSE, add_tapped, p);
  /* each time the page shows (also back from a person, who may be gone): the list again */
  g_signal_connect (page, "showing", G_CALLBACK (list_showing), p);
  list_load (p);
  ns_push (page);
}

static void
direct_ui_loaded (JsonObject *root, const char *err, gpointer data)
{
  if (!problem (root, err))
    ns_page_people (NULL, root);   /* it keeps its own ref of the texts */
}

void
ns_page_people_open (GtkWidget *row, gpointer data)
{
  ns_page_minerva (NULL, NULL);   /* `--page people`: the Minerva index under it */
  static const char *const args[] = { "people", "ui", "--json", NULL };
  ns_mc_run (G_OBJECT (g_application_get_default ()), args, NULL, direct_ui_loaded, NULL);
}

/* the index's own Persone row: the index is already under the page */
void
ns_page_people_index (GtkWidget *row, gpointer data)
{
  static const char *const args[] = { "people", "ui", "--json", NULL };
  ns_mc_run (G_OBJECT (g_application_get_default ()), args, NULL, direct_ui_loaded, NULL);
}
