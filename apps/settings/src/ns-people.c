/* Minerva > Persone: the people Minerva knows (Minerva's people/ store), each with a notebook, face samples, a
 * voiceprint and a linked contact. Every label comes from `minerva-config people ui` (data:
 * config/minerva_config/people_ui.json); everything is read and written by `minerva-config people … --json`, which
 * answers {"ok": false, "error": …} for what it refused: the page shows that message and changes nothing.
 * The list is `people known`: one archive of people, pets, objects and places, a card per kind in the order the data
 * gives them, strangers in a card of their own under their kind. */
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

static double
dbl_of (JsonObject *o, const char *member)
{
  return o && json_object_has_member (o, member) ? json_object_get_double_member_with_default (o, member, 0) : 0;
}

/* a text of one of the data's maps ("kinds", "kind_one", "kind_icons"…) for `key`, `fallback` when it has none */
static const char *
map_str (JsonObject *ui, const char *map, const char *key, const char *fallback)
{
  JsonObject *m = obj_of (ui, map);
  const char *v = m && key ? str_of (m, key) : NULL;
  return v && *v ? v : fallback;
}

static const char *
kind_icon (JsonObject *ui, const char *kind)
{
  return map_str (ui, "kind_icons", kind, "avatar-default-symbolic");
}

static const char *
kind_color (JsonObject *ui, const char *kind)
{
  return map_str (ui, "kind_colors", kind, "blue");
}

/* a date of the entry (ISO 8601 text or Unix time) in the data's date_format; NULL when it has none */
static char *
seen_of (JsonObject *ui, JsonObject *o, const char *member)
{
  JsonNode *n = o ? json_object_get_member (o, member) : NULL;
  if (!n || JSON_NODE_TYPE (n) != JSON_NODE_VALUE)
    return NULL;
  const char *df = str_of (obj_of (ui, "list"), "date_format");
  g_autoptr (GDateTime) t = NULL;
  if (json_node_get_value_type (n) == G_TYPE_STRING) {
    const char *v = json_node_get_string (n);
    if (!v || !*v)
      return NULL;
    g_autoptr (GTimeZone) tz = g_time_zone_new_local ();
    t = g_date_time_new_from_iso8601 (v, tz);
    if (!t) {
      g_autofree char *full = g_strconcat (v, "T00:00:00", NULL);
      t = g_date_time_new_from_iso8601 (full, tz);
    }
    if (!t)
      return g_strdup (v);
  } else {
    double v = json_node_get_double (n);
    if (v <= 0)
      return NULL;
    if (v > 1e11)
      v /= 1000;   /* milliseconds */
    t = g_date_time_new_from_unix_local ((gint64) v);
  }
  return t ? g_date_time_format (t, df && *df ? df : "%x") : NULL;
}

static gint64
pct_of (JsonObject *o)
{
  return (gint64) (dbl_of (o, "confidence") * 100 + 0.5);
}

/* an entry's subtitle in the list (and in the Unisci picker): owner • samples, sightings, confidence • last seen •
 * voice • contact • notes */
static char *
entry_sub (JsonObject *ui, JsonObject *o)
{
  JsonObject *l = obj_of (ui, "list");
  g_autofree char *samples = g_strdup_printf ("%" G_GINT64_FORMAT, int_of (o, "samples"));
  g_autofree char *sightings = g_strdup_printf ("%" G_GINT64_FORMAT, int_of (o, "sightings"));
  g_autofree char *conf = g_strdup_printf ("%" G_GINT64_FORMAT, pct_of (o));
  g_autofree char *row = fmt (str_of (l, "row"), "samples", samples, "sightings", sightings, "confidence", conf, NULL);
  g_autofree char *date = seen_of (ui, o, "last_seen");
  g_autofree char *seen = date ? fmt (str_of (l, "last_seen"), "date", date, NULL) : g_strdup (str_of (l, "never_seen"));
  g_autofree char *notes = fmt_n (str_of (l, int_of (o, "notes") == 1 ? "notes_one" : "notes"), "n", int_of (o, "notes"));
  return g_strdup_printf ("%s%s%s • %s • %s • %s • %s", bool_of (o, "owner") ? str_of (l, "owner") : "",
                          bool_of (o, "owner") ? " • " : "", row, seen,
                          str_of (l, bool_of (o, "voiceprint") ? "voice_yes" : "voice_no"),
                          str_of (l, *str_of (o, "contact_uid") ? "contact_yes" : "contact_no"), notes);
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
  gboolean    enrolled, running;
  gboolean    learning, learn_hold;   /* voice_learn.enabled of the answer; hold: the user's toggle is in flight */
  gint64      clips, need;            /* voice_learn.clips / .need */
  GtkWidget  *msg, *faces_row, *voice_row, *enroll_btn, *forget_btn, *contact_row, *unlink_btn;
  GtkWidget  *learn_row, *learn_sw, *clips_btn;
  guint       poll_id, poll_s;
  char       *kind, *pending_into, *pending_into_name;
  JsonObject *known, *shown;          /* the last `people known` and `people show` answers */
  GPtrArray  *kind_ids;               /* the kinds of the Tipo picker, in its order */
  GtkWidget  *manage_g, *rename_row, *kind_row, *merge_row;
  GtkWidget  *nb_row, *nb_clear, *nb_note, *nb_closed;
  GtkWidget  *st_samples, *st_sightings, *st_conf, *st_first, *st_last;
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
  g_free (d->kind);
  g_free (d->pending_into);
  g_free (d->pending_into_name);
  if (d->known)
    json_object_unref (d->known);
  if (d->shown)
    json_object_unref (d->shown);
  if (d->kind_ids)
    g_ptr_array_unref (d->kind_ids);
  g_free (d);
}

static void voice_load (Pd *d);
static void person_load (Pd *d);
static void open_notebook (GtkWidget *row, gpointer data);
static void known_load (Pd *d);
static void kind_apply (Pd *d);
static void rules_open (JsonObject *ui);

static void
d_reload (Pd *d)
{
  person_load (d);
  known_load (d);
}

/* the read-only stats from the last answers (max_samples is a rule of `people known`) */
static void
stats_apply (Pd *d)
{
  JsonObject *o = d->shown;
  if (!o)
    return;
  gint64 max = int_of (obj_of (d->known, "rules"), "max_samples");
  gint64 n = json_object_has_member (o, "samples") ? int_of (o, "samples") : int_of (o, "faces");
  g_autofree char *ns = g_strdup_printf ("%" G_GINT64_FORMAT, n);
  g_autofree char *ms = g_strdup_printf ("%" G_GINT64_FORMAT, max);
  g_autofree char *sv = max > 0 ? fmt (d_str (d, "stat_samples_value"), "n", ns, "max", ms, NULL) : g_strdup (ns);
  ns_row_set_subtitle (d->st_samples, sv);
  g_autofree char *sg = fmt_n (d_str (d, "stat_sightings_value"), "n", int_of (o, "sightings"));
  ns_row_set_subtitle (d->st_sightings, sg);
  g_autofree char *cf = fmt_n (d_str (d, "stat_confidence_value"), "n", pct_of (o));
  ns_row_set_subtitle (d->st_conf, cf);
  g_autofree char *first = seen_of (d->ui, o, "first_seen");
  g_autofree char *last = seen_of (d->ui, o, "last_seen");
  ns_row_set_subtitle (d->st_first, first ? first : d_str (d, "stat_never"));
  ns_row_set_subtitle (d->st_last, last ? last : d_str (d, "stat_never"));
}

/* the learning row from the last answers: the server's state, never the switch's own (unless a toggle is in flight) */
static void
learn_apply (Pd *d)
{
  if (!d->learn_row)
    return;
  g_autofree char *clips = g_strdup_printf ("%" G_GINT64_FORMAT, d->clips);
  g_autofree char *need = g_strdup_printf ("%" G_GINT64_FORMAT, d->need);
  g_autofree char *sub = NULL;
  if (!d->learning)
    sub = g_strdup (d_str (d, "learn_off"));
  else if (d->clips < d->need)
    sub = fmt (d_str (d, "learn_progress"), "clips", clips, "need", need, NULL);
  else
    sub = fmt (d_str (d, "learn_progress_done"), "clips", clips, NULL);
  ns_row_set_subtitle (d->learn_row, sub);
  gtk_widget_set_sensitive (d->learn_row, d->running);
  if (d->learn_sw) {
    if (!d->learn_hold)
      ns_switch_quiet (d->learn_sw, d->learning);
    gtk_widget_set_sensitive (d->learn_sw, d->running);
  }
  if (d->clips_btn)
    gtk_widget_set_visible (d->clips_btn, d->clips > 0);
}

static gboolean
poll_tick (gpointer data)
{
  Pd *d = data;
  d->poll_id = 0;
  if (d->learning)
    person_load (d);   /* the clip count is in `people show`; it loads the voice service after it */
  else
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
  JsonObject *learn = obj_of (root, "learn");   /* the clips the service keeps (learn.py pos/), and how many it wants */
  d->clips = int_of (learn, "learned");
  d->need = int_of (learn, "needed");
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
  d->running = running;
  learn_apply (d);
  if ((busy || d->learning) && !d->poll_id)
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
  ns_row_set_subtitle (d->rename_row, d->name);
  if (*str_of (root, "kind")) {
    g_free (d->kind);
    d->kind = g_strdup (str_of (root, "kind"));
    kind_apply (d);
  }
  if (d->shown)
    json_object_unref (d->shown);
  d->shown = json_object_ref (root);
  stats_apply (d);
  /* a stranger's notebook is closed until a contact is linked: one note row in place of the editor */
  gboolean nb = json_object_has_member (root, "notebook_open") ? bool_of (root, "notebook_open") : TRUE;
  gtk_widget_set_visible (d->nb_row, nb);
  gtk_widget_set_visible (d->nb_clear, nb);
  gtk_widget_set_visible (d->nb_note, nb);
  gtk_widget_set_visible (d->nb_closed, !nb);
  gint64 faces = json_object_has_member (root, "samples") ? int_of (root, "samples") : int_of (root, "faces");
  g_autofree char *fv = faces ? fmt_n (d_str (d, "faces_value"), "n", faces) : g_strdup (d_str (d, "faces_none"));
  ns_row_set_subtitle (d->faces_row, fv);
  d->enrolled = bool_of (root, "voiceprint");
  gboolean linked = *str_of (root, "contact_uid") != '\0';
  const char *cname = *str_of (root, "contact_name") ? str_of (root, "contact_name") : str_of (root, "contact_uid");
  ns_row_set_subtitle (d->contact_row, linked ? cname : d_str (d, "contact_none"));
  gtk_widget_set_visible (d->unlink_btn, linked);
  d->learning = bool_of (obj_of (root, "voice_learn"), "enabled");
  learn_apply (d);
  voice_load (d);
}

static void
person_load (Pd *d)
{
  const char *const show[] = { "people", "show", d->id, "--json", NULL };
  ns_mc_run (d->owner, show, NULL, person_loaded, d);
}

/* an action's answer: the message of the data (`done` key of "detail") or its problem, then the page again */
typedef struct { Pd *d; const char *done; gboolean pop; } Ans;

static void
answered (JsonObject *root, const char *err, gpointer data)
{
  Ans *a = data;
  Pd *d = a->d;
  const char *why = problem (root, err);
  d->learn_hold = FALSE;
  if (why) {
    msg_set (d->msg, why, TRUE);
    learn_apply (d);   /* a refused toggle: the switch back to what the server holds */
    kind_apply (d);    /* a refused kind: the picker back to the entry's */
  } else if (a->pop)
    gtk_widget_activate_action (d->msg, "navigation.pop", NULL);
  else {
    msg_set (d->msg, a->done ? d_str (d, a->done) : "", FALSE);
    d_reload (d);
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
learn_consent_given (gboolean yes, gpointer data)
{
  Pd *d = data;
  if (!yes) {
    d->learn_hold = FALSE;
    learn_apply (d);
    return;
  }
  /* the consent is the confirmation the user read and accepted, word for word */
  g_autofree char *consent = fmt (d_str (d, "learn_consent_body"), "name", d->name, NULL);
  const char *const args[] = { "people", "voice-learn", d->id, "on", "--consent", consent, "--json", NULL };
  run (d, args, NULL, "learn_enabled", FALSE);
}

static void
learn_toggled (GObject *sw, GParamSpec *pspec, gpointer data)
{
  Pd *d = data;
  if (ns_switch_syncing (GTK_WIDGET (sw)))
    return;
  d->learn_hold = TRUE;
  if (gtk_switch_get_active (GTK_SWITCH (sw))) {
    g_autofree char *heading = fmt (d_str (d, "learn_consent_heading"), "name", d->name, NULL);
    g_autofree char *body = fmt (d_str (d, "learn_consent_body"), "name", d->name, NULL);
    ns_confirm (GTK_WIDGET (sw), heading, body, d_str (d, "learn_consent_ok"), FALSE, learn_consent_given, d);
  } else {
    const char *const args[] = { "people", "voice-learn", d->id, "off", "--json", NULL };
    run (d, args, NULL, "learn_disabled", FALSE);
  }
}

static void
clips_confirmed (gboolean yes, gpointer data)
{
  Pd *d = data;
  const char *const args[] = { "people", "voice-clips", d->id, "clear", "--json", NULL };
  if (yes)
    run (d, args, NULL, "clips_forgotten", FALSE);
}

static void
clips_tapped (GtkWidget *row, gpointer data)
{
  Pd *d = data;
  g_autofree char *clips = g_strdup_printf ("%" G_GINT64_FORMAT, d->clips);
  g_autofree char *body = fmt (d_str (d, "clips_forget_body"), "clips", clips, "name", d->name, NULL);
  ns_confirm (row, d_str (d, "clips_forget_heading"), body, d_str (d, "clips_forget"), TRUE, clips_confirmed, d);
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

/* ---- Nome, Tipo, Unisci ---- */

static void
rename_entered (const char *text, gpointer data)
{
  Pd *d = data;
  g_autofree char *name = g_strstrip (g_strdup (text ? text : ""));
  if (!*name || !g_strcmp0 (name, d->name))
    return;
  const char *const args[] = { "people", "rename", d->id, name, "--json", NULL };
  run (d, args, NULL, "renamed", FALSE);
}

static void
rename_tapped (GtkWidget *row, gpointer data)
{
  Pd *d = data;
  g_autofree char *body = fmt (d_str (d, "rename_body"), "name", d->name, NULL);
  ns_ask (row, d_str (d, "rename_heading"), body, d->name, FALSE, d_str (d, "rename_ok"), rename_entered, d);
}

/* the Tipo picker on the entry's kind (ns_row_pick_select: the callback sees the same kind and does nothing) */
static void
kind_apply (Pd *d)
{
  if (!d->kind_row || !d->kind_ids)
    return;
  for (guint i = 0; i < d->kind_ids->len; i++)
    if (!g_strcmp0 (g_ptr_array_index (d->kind_ids, i), d->kind))
      ns_row_pick_select (d->kind_row, i);
}

static void
kind_picked (GtkWidget *row, guint index, gpointer data)
{
  Pd *d = data;
  if (!d->kind_ids || index >= d->kind_ids->len)
    return;
  const char *kind = g_ptr_array_index (d->kind_ids, index);
  if (!g_strcmp0 (kind, d->kind))
    return;
  const char *const args[] = { "people", "kind", d->id, kind, "--json", NULL };
  run (d, args, NULL, "kind_changed", FALSE);
}

typedef struct { Pd *d; GObject *page; GtkWidget *group, *msg; } Pick;   /* the Collega and Unisci pages' */

static void
merge_answered (JsonObject *root, const char *err, gpointer data)
{
  Pick *k = data;
  const char *why = problem (root, err);
  if (why) {
    msg_set (k->msg, why, TRUE);
    return;
  }
  /* the entry is gone: back past its page to the list */
  GtkWidget *nav = gtk_widget_get_ancestor (k->msg, ADW_TYPE_NAVIGATION_VIEW);
  AdwNavigationPage *prev = nav ? adw_navigation_view_get_previous_page (ADW_NAVIGATION_VIEW (nav),
                                                                        ADW_NAVIGATION_PAGE (k->d->owner)) : NULL;
  if (prev)
    adw_navigation_view_pop_to_page (ADW_NAVIGATION_VIEW (nav), prev);
  else
    gtk_widget_activate_action (k->msg, "navigation.pop", NULL);
}

static void
merge_confirmed (gboolean yes, gpointer data)
{
  Pick *k = data;
  const char *const args[] = { "people", "merge", k->d->id, k->d->pending_into, "--json", NULL };
  if (yes)
    ns_mc_run (k->page, args, NULL, merge_answered, k);   /* k lives with that page */
}

static void
into_tapped (GtkWidget *row, gpointer data)
{
  Pick *k = data;
  Pd *d = k->d;
  g_free (d->pending_into);
  g_free (d->pending_into_name);
  d->pending_into = g_strdup (g_object_get_data (G_OBJECT (row), "pp-id"));
  d->pending_into_name = g_strdup (g_object_get_data (G_OBJECT (row), "pp-name"));
  g_autofree char *heading = fmt (d_str (d, "merge_heading"), "name", d->name, "into", d->pending_into_name, NULL);
  g_autofree char *body = fmt (d_str (d, "merge_body"), "name", d->name, "into", d->pending_into_name, NULL);
  ns_confirm (row, heading, body, d_str (d, "merge_ok"), TRUE, merge_confirmed, k);
}

/* the other entries of the same kind, from the last `people known` */
static guint
merge_peers (Pd *d, GtkWidget *group, Pick *k)
{
  JsonArray *people = arr_of (d->known, "people");
  guint n = 0;
  for (guint i = 0; people && i < json_array_get_length (people); i++) {
    JsonObject *o = json_array_get_object_element (people, i);
    if (g_strcmp0 (str_of (o, "kind"), d->kind) || !g_strcmp0 (str_of (o, "id"), d->id))
      continue;
    n++;
    if (!group)
      continue;
    g_autofree char *sub = entry_sub (d->ui, o);
    GtkWidget *row = ns_row (group, kind_icon (d->ui, d->kind), kind_color (d->ui, d->kind), str_of (o, "name"), sub);
    g_object_set_data_full (G_OBJECT (row), "pp-id", g_strdup (str_of (o, "id")), g_free);
    g_object_set_data_full (G_OBJECT (row), "pp-name", g_strdup (str_of (o, "name")), g_free);
    ns_row_on_activate (row, into_tapped, k);
  }
  return n;
}

static void
merge_tapped (GtkWidget *row, gpointer data)
{
  Pd *d = data;
  g_autofree char *title = fmt (d_str (d, "merge_title"), "name", d->name, NULL);
  GtkWidget *c;
  AdwNavigationPage *page = ns_page_new (title, &c);
  Pick *k = g_new0 (Pick, 1);
  k->d = d;   /* the entry's page stays below this one while it is shown */
  k->page = G_OBJECT (page);
  g_object_set_data_full (G_OBJECT (page), "pp-pick", k, g_free);
  k->msg = msg_new (c);
  k->group = ns_group (c, NULL);
  merge_peers (d, k->group, k);
  g_autofree char *note = fmt (d_str (d, "merge_note"), "name", d->name, NULL);
  ns_note (c, note);
  ns_push (page);
}

static void
known_loaded (JsonObject *root, const char *err, gpointer data)
{
  Pd *d = data;
  const char *why = problem (root, err);
  if (why) {
    msg_set (d->msg, why, TRUE);
    return;
  }
  if (d->known)
    json_object_unref (d->known);
  d->known = json_object_ref (root);
  JsonArray *people = arr_of (root, "people");
  for (guint i = 0; people && i < json_array_get_length (people); i++) {
    JsonObject *o = json_array_get_object_element (people, i);
    if (!g_strcmp0 (str_of (o, "id"), d->id) && *str_of (o, "kind")) {
      g_free (d->kind);
      d->kind = g_strdup (str_of (o, "kind"));
    }
  }
  if (!d->kind_row) {   /* the kinds do not change while the page lives: the picker is built once */
    JsonArray *kinds = arr_of (root, "kinds");
    d->kind_ids = g_ptr_array_new_with_free_func (g_free);
    g_autoptr (GPtrArray) labels = g_ptr_array_new ();
    guint sel = 0;
    for (guint i = 0; kinds && i < json_array_get_length (kinds); i++) {
      const char *id = str_of (json_array_get_object_element (kinds, i), "id");
      if (!g_strcmp0 (id, d->kind))
        sel = d->kind_ids->len;
      g_ptr_array_add (d->kind_ids, g_strdup (id));
      g_ptr_array_add (labels, (gpointer) map_str (d->ui, "kind_one", id, id));
    }
    g_ptr_array_add (labels, NULL);
    d->kind_row = ns_row_pick (d->manage_g, d_str (d, "kind_row"), (const char *const *) labels->pdata, sel,
                               kind_picked, d);
    d->merge_row = ns_row (d->manage_g, "edit-copy-symbolic", "violet", d_str (d, "merge_row"),
                           d_str (d, "merge_sub"));
    ns_row_on_activate (d->merge_row, merge_tapped, d);
  }
  kind_apply (d);
  gtk_widget_set_visible (d->merge_row, merge_peers (d, NULL, NULL) > 0);
  stats_apply (d);
}

static void
known_load (Pd *d)
{
  static const char *const args[] = { "people", "known", "--json", NULL };
  ns_mc_run (d->owner, args, NULL, known_loaded, d);
}

static void
d_rules_tapped (GtkWidget *row, gpointer data)
{
  rules_open (((Pd *) data)->ui);
}

/* ---- Collega: the address book's contacts with a similar name, a tap links one (asked first) ---- */

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
  d_reload (data);
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

  /* Nome, then Tipo and Unisci once `people known` answers */
  d->manage_g = ns_group (c, d_str (d, "manage"));
  d->rename_row = ns_row (d->manage_g, "document-edit-symbolic", "grey", d_str (d, "rename_row"), name);
  ns_row_on_activate (d->rename_row, rename_tapped, d);

  g_autofree char *nb_label = fmt (d_str (d, "notebook_label"), "name", name, NULL);
  GtkWidget *g = ns_group (c, d_str (d, "notebook"));
  d->nb_row = ns_row (g, "document-edit-symbolic", "blue", d_str (d, "open"), nb_label);
  ns_row_on_activate (d->nb_row, open_notebook, d);
  d->nb_clear = ns_row_button (g, d_str (d, "clear"), TRUE, clear_tapped, d);
  d->nb_closed = ns_row (g, NULL, NULL, d_str (d, "notebook_closed"), NULL);
  gtk_widget_set_visible (d->nb_closed, FALSE);
  d->nb_note = ns_note (c, d_str (d, "notebook_note"));

  g = ns_group (c, d_str (d, "stats"));
  d->st_samples = ns_row (g, NULL, NULL, d_str (d, "stat_samples"), "");
  d->st_sightings = ns_row (g, NULL, NULL, d_str (d, "stat_sightings"), "");
  d->st_conf = ns_row (g, NULL, NULL, d_str (d, "stat_confidence"), "");
  d->st_first = ns_row (g, NULL, NULL, d_str (d, "stat_first"), "");
  d->st_last = ns_row (g, NULL, NULL, d_str (d, "stat_last"), "");

  g = ns_group (c, d_str (d, "faces"));
  d->faces_row = ns_row (g, "camera-photo-symbolic", "orange", d_str (d, "faces_row"), "");
  ns_note (c, d_str (d, "faces_note"));

  g = ns_group (c, d_str (d, "voice"));
  d->voice_row = ns_row (g, "audio-input-microphone-symbolic", "violet", d_str (d, "voice_row"), "");
  d->enroll_btn = ns_row_button (g, d_str (d, "voice_enroll"), FALSE, enroll_tapped, d);
  d->forget_btn = ns_row_button (g, d_str (d, "voice_forget"), TRUE, forget_tapped, d);
  d->learn_row = ns_row_toggle (g, "audio-input-microphone-symbolic", "violet", d_str (d, "learn_row"),
                                d_str (d, "learn_off"));
  d->learn_sw = ns_row_get_switch (d->learn_row);
  if (d->learn_sw)
    g_signal_connect (d->learn_sw, "notify::active", G_CALLBACK (learn_toggled), d);
  d->clips_btn = ns_row_button (g, d_str (d, "clips_forget"), TRUE, clips_tapped, d);
  gtk_widget_set_visible (d->clips_btn, FALSE);
  learn_apply (d);   /* insensitive until the voice service answers */

  g = ns_group (c, d_str (d, "contact"));
  d->contact_row = ns_row (g, "contact-new-symbolic", "green", d_str (d, "contact_row"), "");
  ns_row_button (g, d_str (d, "link"), FALSE, link_tapped, d);
  d->unlink_btn = ns_row_button (g, d_str (d, "unlink"), TRUE, unlink_tapped, d);

  g = ns_group (c, NULL);
  GtkWidget *rules = ns_row (g, "preferences-system-symbolic", "grey", d_str (d, "rules"), d_str (d, "rules_sub"));
  ns_row_on_activate (rules, d_rules_tapped, d);

  g = ns_group (c, NULL);
  ns_row_button (g, d_str (d, "delete"), TRUE, delete_tapped, d);

  /* each time the page shows (also back from Collega): the person again */
  g_signal_connect (page, "showing", G_CALLBACK (person_showing), d);
  ns_push (page);
}

/* ======================= Regole ======================= */

typedef struct {
  GObject    *owner;
  JsonObject *ui;
  GtkWidget  *msg, *days, *step, *start, *ceil, *sweep;
} Rp;

static const char *
r_str (Rp *r, const char *key)
{
  return str_of (obj_of (r->ui, "detail"), key);
}

static void
rp_free (Rp *r)
{
  json_object_unref (r->ui);
  g_free (r);
}

static GtkWidget *
rules_spin (GtkWidget *g, const char *title, double lo, double hi, double step, guint digits)
{
  GtkWidget *s = gtk_spin_button_new_with_range (lo, hi, step);
  gtk_spin_button_set_digits (GTK_SPIN_BUTTON (s), digits);
  gtk_widget_set_sensitive (s, FALSE);   /* until the rules arrive */
  ns_row_widget (g, title, s);
  return s;
}

static void
rules_apply (Rp *r, JsonObject *rules)
{
  if (!rules)
    return;
  gtk_spin_button_set_value (GTK_SPIN_BUTTON (r->days), (double) int_of (rules, "forget_days"));
  gtk_spin_button_set_value (GTK_SPIN_BUTTON (r->step), dbl_of (rules, "sight_step"));
  gtk_spin_button_set_value (GTK_SPIN_BUTTON (r->start), dbl_of (rules, "conf_start"));
  gtk_spin_button_set_value (GTK_SPIN_BUTTON (r->ceil), dbl_of (rules, "conf_ceil"));
  GtkWidget *spins[] = { r->days, r->step, r->start, r->ceil };
  for (guint i = 0; i < G_N_ELEMENTS (spins); i++)
    gtk_widget_set_sensitive (spins[i], TRUE);
  g_autofree char *sweep = fmt_n (r_str (r, "rules_sweep"), "days", int_of (rules, "forget_days"));
  g_autofree char *text = g_strdup_printf ("%s %s", sweep,
                                           r_str (r, bool_of (rules, "sweep_log_only") ? "rules_sweep_log"
                                                                                      : "rules_sweep_apply"));
  gtk_label_set_text (GTK_LABEL (r->sweep), text);
}

static void
rules_loaded (JsonObject *root, const char *err, gpointer data)
{
  Rp *r = data;
  const char *why = problem (root, err);
  msg_set (r->msg, why, TRUE);
  if (!why)
    rules_apply (r, obj_of (root, "rules"));
}

static void
rules_saved (JsonObject *root, const char *err, gpointer data)
{
  Rp *r = data;
  const char *why = problem (root, err);
  if (why) {
    msg_set (r->msg, why, TRUE);
    return;
  }
  msg_set (r->msg, r_str (r, "rules_saved"), FALSE);
  JsonObject *rules = obj_of (root, "rules");
  rules_apply (r, rules ? rules : root);
}

/* key=value with a dot whatever the locale (Italian writes 0,05) */
static char *
rule_arg (const char *key, GtkWidget *spin)
{
  char buf[G_ASCII_DTOSTR_BUF_SIZE];
  g_ascii_formatd (buf, sizeof buf, "%.2f", gtk_spin_button_get_value (GTK_SPIN_BUTTON (spin)));
  return g_strconcat (key, "=", buf, NULL);
}

static void
rules_save (GtkWidget *row, gpointer data)
{
  Rp *r = data;
  g_autofree char *days = g_strdup_printf ("forget_days=%d", gtk_spin_button_get_value_as_int (GTK_SPIN_BUTTON (r->days)));
  g_autofree char *step = rule_arg ("sight_step", r->step);
  g_autofree char *start = rule_arg ("conf_start", r->start);
  g_autofree char *ceil = rule_arg ("conf_ceil", r->ceil);
  const char *const args[] = { "people", "rules-set", days, step, start, ceil, "--json", NULL };
  ns_mc_run (r->owner, args, NULL, rules_saved, r);
}

/* a pushed page of its own, reached from the list and from an entry: not in the Minerva index */
static void
rules_open (JsonObject *ui)
{
  GtkWidget *c;
  JsonObject *det = obj_of (ui, "detail");
  AdwNavigationPage *page = ns_page_new (str_of (det, "rules_title"), &c);
  Rp *r = g_new0 (Rp, 1);
  r->owner = G_OBJECT (page);
  r->ui = json_object_ref (ui);
  g_object_set_data_full (G_OBJECT (page), "pp-rules", r, (GDestroyNotify) rp_free);
  r->msg = msg_new (c);
  GtkWidget *g = ns_group (c, r_str (r, "rules_group"));
  gint64 max = int_of (det, "rules_days_max");
  r->days = rules_spin (g, r_str (r, "rules_forget_days"), 1, max > 1 ? (double) max : 1, 1, 0);
  r->step = rules_spin (g, r_str (r, "rules_sight_step"), 0, 1, 0.01, 2);
  r->start = rules_spin (g, r_str (r, "rules_conf_start"), 0, 1, 0.01, 2);
  r->ceil = rules_spin (g, r_str (r, "rules_conf_ceil"), 0, 1, 0.01, 2);
  ns_note (c, r_str (r, "rules_note"));
  g = ns_group (c, NULL);
  ns_row_button (g, r_str (r, "rules_save"), FALSE, rules_save, r);
  r->sweep = ns_note (c, "");
  static const char *const args[] = { "people", "known", "--json", NULL };
  ns_mc_run (G_OBJECT (page), args, NULL, rules_loaded, r);
  ns_push (page);
}

/* ======================= the list ======================= */

typedef struct {
  GObject    *owner;
  JsonObject *ui;
  GtkWidget  *msg, *kinds_box;   /* kinds_box: the kinds' cards, built again at each load */
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
entry_row (Pp *p, GtkWidget *group, JsonObject *o)
{
  const char *kind = str_of (o, "kind");
  g_autofree char *sub = entry_sub (p->ui, o);
  GtkWidget *row = ns_row (group, kind_icon (p->ui, kind), kind_color (p->ui, kind), str_of (o, "name"), sub);
  g_object_set_data_full (G_OBJECT (row), "pp-id", g_strdup (str_of (o, "id")), g_free);
  g_object_set_data_full (G_OBJECT (row), "pp-name", g_strdup (str_of (o, "name")), g_free);
  ns_row_on_activate (row, person_tapped, p);
}

static void
list_loaded (JsonObject *root, const char *err, gpointer data)
{
  Pp *p = data;
  const char *why = problem (root, err);
  msg_set (p->msg, why, TRUE);
  if (why)
    return;
  GtkWidget *ch;
  while ((ch = gtk_widget_get_first_child (p->kinds_box)))
    gtk_box_remove (GTK_BOX (p->kinds_box), ch);
  JsonArray *kinds = arr_of (root, "kinds");
  JsonArray *people = arr_of (root, "people");
  guint nk = kinds ? json_array_get_length (kinds) : 0;
  guint np = people ? json_array_get_length (people) : 0;
  gint64 days = int_of (obj_of (root, "rules"), "forget_days");
  g_autofree gboolean *placed = g_new0 (gboolean, np + 1);
  /* a card per kind in the data's order; its strangers in a card of their own right under it */
  for (guint k = 0; k < nk; k++) {
    const char *kid = str_of (json_array_get_object_element (kinds, k), "id");
    GtkWidget *g = ns_group (p->kinds_box, map_str (p->ui, "kinds", kid, kid));
    guint shown = 0, strangers = 0;
    for (guint i = 0; i < np; i++) {
      JsonObject *o = json_array_get_object_element (people, i);
      if (g_strcmp0 (str_of (o, "kind"), kid))
        continue;
      placed[i] = TRUE;
      if (bool_of (o, "stranger")) {
        strangers++;
        continue;
      }
      entry_row (p, g, o);
      shown++;
    }
    if (!shown)
      ns_row (g, NULL, NULL, l_str (p, !np && !k ? "empty" : "empty_kind"), NULL);
    if (!strangers)
      continue;
    GtkWidget *sg = ns_group (p->kinds_box, l_str (p, "strangers"));
    for (guint i = 0; i < np; i++) {
      JsonObject *o = json_array_get_object_element (people, i);
      if (!g_strcmp0 (str_of (o, "kind"), kid) && bool_of (o, "stranger"))
        entry_row (p, sg, o);
    }
    g_autofree char *note = fmt_n (l_str (p, "stranger_note"), "days", days);
    ns_note (p->kinds_box, note);
  }
  /* entries of a kind the data does not list: kept in sight */
  GtkWidget *other = NULL;
  for (guint i = 0; i < np; i++) {
    if (placed[i])
      continue;
    if (!other)
      other = ns_group (p->kinds_box, l_str (p, "other"));
    entry_row (p, other, json_array_get_object_element (people, i));
  }
}

static void
rules_tapped (GtkWidget *row, gpointer data)
{
  rules_open (((Pp *) data)->ui);
}

static void
list_load (Pp *p)
{
  static const char *const args[] = { "people", "known", "--json", NULL };
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
  p->kinds_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_append (GTK_BOX (c), p->kinds_box);
  ns_note (c, l_str (p, "note"));
  GtkWidget *g = ns_group (c, NULL);
  GtkWidget *rules = ns_row (g, "preferences-system-symbolic", "grey", l_str (p, "rules"), l_str (p, "rules_sub"));
  ns_row_on_activate (rules, rules_tapped, p);
  g = ns_group (c, NULL);
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

static void
open_notebook (GtkWidget *row, gpointer data)
{
  Pd *d = data;
  ns_prompt_open ("notebook", d->id);
}

void
ns_page_people_index (GtkWidget *row, gpointer data)
{
  static const char *const args[] = { "people", "ui", "--json", NULL };
  ns_mc_run (G_OBJECT (g_application_get_default ()), args, NULL, direct_ui_loaded, NULL);
}
