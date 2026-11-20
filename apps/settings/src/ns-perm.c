/* Security and privacy > Permissions: what Minerva's agent may do without asking. A viewer and editor of
 * `minerva-perm` (permissions/ in Minerva's repository, docs/permissions.md): `status --json` for the mode, the hard
 * gates, the base allow and the rules, `history -n 30 --json` for the decisions, and `mode`, `rule add`, `rule rm`,
 * `classifier set` to change them. Every sentence in History comes from the data, verbatim. The first card is her
 * permission to control the phone's screen (`status.screen_control`, `screen grant|revoke`): its words, its state and
 * the date it was granted all come from the data, and turning it on asks on the phone's own sheet (Sì, then a finger
 * or the PIN), not here. */
#include "msg-assistant.h"
#include "ns-pages.h"
#include "ns-mc.h"
#include "ns-ui.h"


#define str_of ns_json_str
#define arr_of ns_json_arr
#define obj_of ns_json_obj

typedef struct Perm Perm;

struct Perm {
  AdwNavigationPage *page;
  GtkWidget *msg;                  /* the last result: saved, or the error */
  GtkWidget *ask_mark, *auto_mark; /* the two mode rows: the chosen one's tick is shown */
  GtkWidget *gates, *base, *rules, *history, *fallbacks;
  GtkWidget *rules_none, *history_none, *model_row;
  GtkWidget *rem_count, *rem_interval, *rem_final; /* the reminders rows: values of `status.reminders` */
  GtkWidget *screen_row, *screen_note; /* the screen permission: the row with its switch, the explanation under it */
  gboolean screen_busy;            /* a grant or a revoke is running: the switch waits for it */
  JsonObject *st;                  /* the last `status --json` */
  gboolean busy;
};

static void perm_refresh (Perm *ak);

/* ---- the message line ---- */

static void
say (Perm *ak, const char *text, gboolean error)
{
  gtk_label_set_text (GTK_LABEL (ak->msg), text);
  gtk_widget_set_visible (ak->msg, text && *text);
  if (error)
    gtk_widget_add_css_class (ak->msg, "ns-danger");
  else
    gtk_widget_remove_css_class (ak->msg, "ns-danger");
}

/* ---- running a change ---- */

typedef struct {
  Perm      *ak;
  char      *id;     /* a rule's id, freed with the dialog that asked about it */
  gboolean   pop;    /* back to the list when it worked (the Add rule form) */
  GtkWidget *from;
} Op;

static void
op_free (Op *op)
{
  g_free (op->id);
  g_free (op);
}

static void
op_done (JsonObject *root, const char *err, gpointer data)
{
  Op *op = data;
  Perm *ak = op->ak;
  ak->busy = FALSE;
  if (err) {
    g_autofree char *t = g_strdup_printf ("Not changed: %s", err);
    say (ak, t, TRUE);
  } else {
    say (ak, "Saved.", FALSE);
    if (op->pop && op->from)
      gtk_widget_activate_action (op->from, "navigation.pop", NULL);
  }
  perm_refresh (ak);
  op_free (op);
}

static void
perm_run_ex (Perm *ak, const char *const *args, gboolean pop, GtkWidget *from)
{
  if (ak->busy) {
    say (ak, "Still applying the last change…", FALSE);
    return;
  }
  ak->busy = TRUE;
  say (ak, "Applying…", FALSE);
  Op *op = g_new0 (Op, 1);
  op->ak = ak;
  op->pop = pop;
  op->from = from;
  /* the form's page holds the answer it waits for, so it is still there when the change is done */
  ns_mc_run_bin (pop && from ? G_OBJECT (from) : G_OBJECT (ak->page), "minerva-perm", args, NULL, op_done, op);
}

static void
perm_run (Perm *ak, const char *const *args)
{
  perm_run_ex (ak, args, FALSE, NULL);
}

/* ---- the mode ---- */

static const char *
mode_of (Perm *ak)
{
  const char *m = str_of (ak->st, "mode");
  return *m ? m : "auto";
}

static void
mode_set (Perm *ak, const char *mode)
{
  if (!g_strcmp0 (mode_of (ak), mode))
    return;
  const char *const args[] = { "mode", mode, NULL };
  perm_run (ak, args);
}

static void
ask_clicked (GtkWidget *row, gpointer data)
{
  mode_set (data, "ask");
}

static void
auto_clicked (GtkWidget *row, gpointer data)
{
  mode_set (data, "auto");
}

/* the tick that says which mode is on */
static GtkWidget *
mark_suffix (GtkWidget *row)
{
  GtkWidget *mark = gtk_image_new_from_icon_name ("object-select-symbolic");
  gtk_widget_add_css_class (mark, "ns-accent");
  gtk_widget_set_opacity (mark, 0);
  ns_row_suffix (row, mark);
  return mark;
}

/* ---- control of the phone's screen: the row's words are `status.screen_control`, never text of this file ---- */

static void
screen_fill (Perm *ak)
{
  JsonObject *sc = obj_of (ak->st, "screen_control");
  const char *state = str_of (sc, "state");
  if (ak->screen_busy)
    return;
  g_autofree char *sub = g_strdup_printf ("%s • %s", str_of (sc, "state_label"), str_of (sc, "granted_text"));
  gtk_label_set_text (GTK_LABEL (g_object_get_data (G_OBJECT (ak->screen_row), "ns-title-label")),
                      str_of (sc, "label"));
  ns_row_set_subtitle (ak->screen_row, sub);
  GtkWidget *sw = ns_row_get_switch (ak->screen_row);
  ns_switch_quiet (sw, !g_strcmp0 (state, "on"));
  gtk_widget_set_sensitive (sw, g_strcmp0 (state, "unavailable") != 0);
  gtk_label_set_text (GTK_LABEL (ak->screen_note), str_of (sc, "explanation"));
  gtk_widget_set_visible (ak->screen_note, *str_of (sc, "explanation"));
}

static void
screen_done (JsonObject *root, const char *err, gpointer data)
{
  Perm *ak = data;
  ak->screen_busy = FALSE;
  /* a refusal is the plain sentence the tool printed after "error: " */
  say (ak, err && g_str_has_prefix (err, "error: ") ? err + 7 : err ? err : "", err != NULL);
  perm_refresh (ak);
}

static void
screen_toggled (GtkSwitch *sw, GParamSpec *ps, gpointer data)
{
  Perm *ak = data;
  if (ns_switch_syncing (GTK_WIDGET (sw)) || ak->screen_busy)
    return;
  gboolean on = gtk_switch_get_active (sw);
  static const char *const grant[] = { "screen", "grant", "--json", NULL };
  static const char *const revoke[] = { "screen", "revoke", "--json", NULL };
  ak->screen_busy = TRUE;
  say (ak, "", FALSE);
  ns_row_set_subtitle (ak->screen_row, str_of (obj_of (ak->st, "screen_control"), on ? "waiting" : "revoking"));
  ns_mc_run_bin (G_OBJECT (ak->page), "minerva-perm", on ? grant : revoke, NULL, screen_done, ak);
}

/* ---- the lists that come from status ---- */

static void
gates_fill (Perm *ak)
{
  ns_group_clear (ak->gates);
  JsonArray *a = arr_of (ak->st, "gates");
  for (guint i = 0; a && i < json_array_get_length (a); i++)
    ns_row (ak->gates, "system-lock-screen-symbolic", "red", str_of (json_array_get_object_element (a, i), "label"),
            "Cannot be turned off");
  ns_group_set_visible (ak->gates, a && json_array_get_length (a) > 0);
}

static void
base_fill (Perm *ak)
{
  ns_group_clear (ak->base);
  JsonArray *a = arr_of (ak->st, "base_allow");
  for (guint i = 0; a && i < json_array_get_length (a); i++)
    ns_row (ak->base, "object-select-symbolic", "green",
            str_of (json_array_get_object_element (a, i), "label"), "Never asks");
  ns_group_set_visible (ak->base, a && json_array_get_length (a) > 0);
}

/* a rule's tool in words */
static const char *
tool_label (const char *tool)
{
  return !g_strcmp0 (tool, "shell") ? "Shell command" :
         !g_strcmp0 (tool, "edit")  ? "File edit" :
         !g_strcmp0 (tool, "read")  ? "File read" :
         !g_strcmp0 (tool, "any")   ? "Any action" : tool;
}

/* ---- the Add rule form ---- */

typedef struct {
  Perm              *ak;
  AdwNavigationPage *page;
  GtkWidget         *effect, *tool, *pattern, *note_row, *msg;
  guint              effect_i, tool_i;
} Form;

static void
form_error (Form *f, const char *text)
{
  gtk_label_set_text (GTK_LABEL (f->msg), text);
  gtk_widget_set_visible (f->msg, TRUE);
}

static void
effect_picked (GtkWidget *row, guint index, gpointer data)
{
  ((Form *) data)->effect_i = index;
}

static void
tool_picked (GtkWidget *row, guint index, gpointer data)
{
  ((Form *) data)->tool_i = index;
}

static const char *const effects[] = { "allow", "deny" };
static const char *const effect_labels[] = { "Allow", "Deny", NULL };
static const char *const tools[] = { "shell", "edit", "read", "any" };
static const char *const tool_labels[] = { "Shell command", "File edit", "File read", "Any action", NULL };

static void
form_submit (GtkWidget *row, gpointer data)
{
  Form *f = data;
  const char *pattern = ns_row_value_get (f->pattern), *note = ns_row_value_get (f->note_row);
  if (!*pattern) {
    form_error (f, "A pattern is needed: for example `git *` or /sdcard/Download/*.");
    return;
  }
  if (f->ak->busy) {
    form_error (f, "Still applying the last change…");
    return;
  }
  g_autoptr (GPtrArray) a = g_ptr_array_new ();
  const char *head[] = { "rule", "add", "--effect", effects[f->effect_i], "--tool", tools[f->tool_i],
                         "--pattern", pattern };
  for (guint i = 0; i < G_N_ELEMENTS (head); i++)
    g_ptr_array_add (a, (char *) head[i]);
  if (*note) {
    g_ptr_array_add (a, (char *) "--note");
    g_ptr_array_add (a, (char *) note);
  }
  g_ptr_array_add (a, NULL);
  gtk_widget_set_visible (f->msg, FALSE);
  perm_run_ex (f->ak, (const char *const *) a->pdata, TRUE, GTK_WIDGET (f->page));
}

static void
add_rule_form (Perm *ak)
{
  GtkWidget *c;
  AdwNavigationPage *page = ns_page_new ("Add rule", &c);
  Form *f = g_new0 (Form, 1);
  f->ak = ak;
  f->page = page;
  g_object_set_data_full (G_OBJECT (page), "perm-form", f, g_free);

  f->msg = ns_note (c, "");
  gtk_widget_set_visible (f->msg, FALSE);
  gtk_widget_add_css_class (f->msg, "ns-danger");
  gtk_widget_set_margin_top (f->msg, 0);

  GtkWidget *g = ns_group (c, NULL);
  f->effect = ns_row_pick (g, "Effect", effect_labels, 0, effect_picked, f);
  f->tool = ns_row_pick (g, "Tool", tool_labels, 0, tool_picked, f);
  f->pattern = ns_row_value (g, "Pattern", "", FALSE, NULL, NULL);
  f->note_row = ns_row_value (g, "Note (optional)", "", FALSE, NULL, NULL);
  ns_note (c, "A shell pattern covers the whole command line, a file one the path; `*` stands for anything, as in "
              "`git -C * status*` or `/sdcard/Music/*`. The note is shown with the rule, for you only.");

  g = ns_group (c, NULL);
  ns_row_button (g, "Add rule", FALSE, form_submit, f);
  ns_push (page);
}

static void
add_rule_opened (GtkWidget *row, gpointer data)
{
  add_rule_form (data);
}

/* ---- revoking a rule ---- */

static void
revoked (gboolean yes, gpointer data)
{
  Op *op = data;
  if (!yes)
    return;
  const char *const args[] = { "rule", "rm", op->id, NULL };
  perm_run (op->ak, args);
}

static void
revoke_clicked (GtkButton *b, gpointer data)
{
  Perm *ak = data;
  const char *id = g_object_get_data (G_OBJECT (b), "perm-id");
  Op *op = g_new0 (Op, 1);
  op->ak = ak;
  op->id = g_strdup (id);
  /* the dialog owns the op: a refresh may rebuild the row while the question is open */
  g_autofree char *again = msg_assistant_fill ("{assistant} asks again (or the classifier decides) for anything this rule "
                                               "covered.");
  AdwDialog *d = ns_confirm (GTK_WIDGET (b), "Revoke this rule?", again, "Revoke", TRUE, revoked, op);
  g_object_set_data_full (G_OBJECT (d), "perm-op", op, (GDestroyNotify) op_free);
}

static void
rules_fill (Perm *ak)
{
  ns_group_clear (ak->rules);
  JsonArray *a = arr_of (ak->st, "rules");
  guint n = a ? json_array_get_length (a) : 0;
  for (guint i = 0; i < n; i++) {
    JsonObject *r = json_array_get_object_element (a, i);
    gboolean allow = g_strcmp0 (str_of (r, "effect"), "deny") != 0;
    const char *pattern = str_of (r, "pattern"), *note = str_of (r, "note");
    g_autofree char *sub = *note
                             ? g_strdup_printf ("%s • %s • %s", allow ? "Allowed" : "Refused",
                                                tool_label (str_of (r, "tool")), note)
                             : g_strdup_printf ("%s • %s", allow ? "Allowed" : "Refused",
                                                tool_label (str_of (r, "tool")));
    GtkWidget *row = ns_row (ak->rules, allow ? "object-select-symbolic" : "changes-prevent-symbolic",
                             allow ? "green" : "red", *pattern ? pattern : "Any", sub);
    GtkWidget *del = gtk_button_new_from_icon_name ("user-trash-symbolic");
    gtk_widget_add_css_class (del, "flat");
    gtk_widget_add_css_class (del, "circular");
    gtk_widget_set_tooltip_text (del, "Revoke");
    g_object_set_data_full (G_OBJECT (del), "perm-id", g_strdup (str_of (r, "id")), g_free);
    g_signal_connect (del, "clicked", G_CALLBACK (revoke_clicked), ak);
    ns_row_suffix (row, del);
  }
  ns_row_button (ak->rules, "Add rule", FALSE, add_rule_opened, ak);
  gtk_widget_set_visible (ak->rules_none, n == 0);
}

/* ---- the classifier ---- */

static void
classifier_model_picked (const char *id, gpointer data)
{
  Perm *ak = data;
  JsonObject *c = obj_of (ak->st, "classifier");
  if (!g_strcmp0 (id, str_of (c, "model")))
    return;
  JsonArray *fb = arr_of (c, "fallbacks");
  char *joined = NULL;
  g_autoptr (GPtrArray) a = g_ptr_array_new ();
  g_ptr_array_add (a, (char *) "classifier");
  g_ptr_array_add (a, (char *) "set");
  g_ptr_array_add (a, (char *) id);
  if (fb && json_array_get_length (fb)) {
    g_autoptr (GString) s = g_string_new ("");
    for (guint i = 0; i < json_array_get_length (fb); i++)
      g_string_append_printf (s, "%s%s", i ? "," : "", json_array_get_string_element (fb, i));
    joined = g_strdup (s->str);
    g_ptr_array_add (a, (char *) "--fallbacks");
    g_ptr_array_add (a, (char *) joined);
  }
  g_ptr_array_add (a, NULL);
  perm_run (ak, (const char *const *) a->pdata);
  g_free (joined);
}

static void
model_clicked (GtkWidget *row, gpointer data)
{
  Perm *ak = data;
  const char *model = str_of (obj_of (ak->st, "classifier"), "model");
  if (!ns_minerva_available ()) {
    say (ak, "The model list needs minerva-config, which is not installed.", TRUE);
    return;
  }
  ns_model_pick (GTK_WIDGET (ak->page), "Classifier model", model, classifier_model_picked, ak);
}

static void
classifier_fill (Perm *ak)
{
  JsonObject *c = obj_of (ak->st, "classifier");
  const char *model = str_of (c, "model");
  ns_row_set_subtitle (ak->model_row, *model ? model : "Not set: unclear requests ask");
  ns_group_clear (ak->fallbacks);
  JsonArray *fb = arr_of (c, "fallbacks");
  guint n = fb ? json_array_get_length (fb) : 0;
  for (guint i = 0; i < n; i++)
    ns_row (ak->fallbacks, NULL, NULL, json_array_get_string_element (fb, i),
            i ? NULL : "Tried when the first cannot answer");
  ns_group_set_visible (ak->fallbacks, n > 0);
}

/* ---- the reminders: an unanswered question is asked again before it counts as a No ---- */

/* `minerva-perm reminders set <flag> <value>` for a row, when the text is a number that makes sense */
static void
rem_set (Perm *ak, const char *flag, const char *text, gboolean whole, double lowest)
{
  char *end = NULL;
  double v = g_ascii_strtod (text, &end);
  if (!*text || *end || v < lowest || (whole && v != (int) v)) {
    const char *t = whole ? "Reminders: a whole number, 0 or more." : "Seconds: a number, 1 or more.";
    say (ak, t, TRUE);
    return;
  }
  const char *const args[] = { "reminders", "set", flag, text, NULL };
  perm_run (ak, args);
}

static void
rem_count_entered (const char *text, gpointer data)
{
  rem_set (data, "--count", text, TRUE, 0);
}

static void
rem_interval_entered (const char *text, gpointer data)
{
  rem_set (data, "--interval", text, FALSE, 1);
}

static void
rem_final_entered (const char *text, gpointer data)
{
  rem_set (data, "--final", text, FALSE, 1);
}

static char *
rem_number (JsonObject *r, const char *key)
{
  if (!r || !json_object_has_member (r, key))
    return g_strdup ("");
  return g_strdup_printf ("%g", json_object_get_double_member (r, key));
}

static void
reminders_fill (Perm *ak)
{
  JsonObject *r = obj_of (ak->st, "reminders");
  g_autofree char *c = rem_number (r, "count"), *i = rem_number (r, "interval_s"), *f = rem_number (r, "final_s");
  ns_row_value_set (ak->rem_count, c);
  ns_row_value_set (ak->rem_interval, i);
  ns_row_value_set (ak->rem_final, f);
}

/* ---- history ---- */

/* "Regola", "Classificatore", "Tu", "Sistema" come from `who`; `by` says the same in the log's own words */
static const char *
who_of (JsonObject *e)
{
  const char *who = str_of (e, "who");
  if (*who)
    return who;
  const char *by = str_of (e, "by");
  return !g_strcmp0 (by, "rule") ? "Regola" :
         !g_strcmp0 (by, "classifier") ? "Classificatore" :
         !g_strcmp0 (by, "user") ? "Tu" : "Sistema";
}

static const char *
who_icon (const char *who)
{
  return !g_strcmp0 (who, "Regola") ? "view-list-symbolic" :
         !g_strcmp0 (who, "Classificatore") ? "system-search-symbolic" :
         !g_strcmp0 (who, "Tu") ? "avatar-default-symbolic" : "preferences-system-symbolic";
}

static const char *
who_color (const char *who)
{
  return !g_strcmp0 (who, "Regola") ? "blue" :
         !g_strcmp0 (who, "Classificatore") ? "green" :
         !g_strcmp0 (who, "Tu") ? "purple" : "grey";
}

static const char *
verdict_of (JsonObject *e)
{
  const char *v = str_of (e, "verdict");
  return !g_strcmp0 (v, "allow") ? "Allowed" :
         !g_strcmp0 (v, "deny") ? "Refused" :
         !g_strcmp0 (v, "ask") ? "Asked" : v;
}

/* a decision's time, near enough for a phone */
static char *
when_of (const char *ts)
{
  g_autoptr (GDateTime) t = ts && *ts ? g_date_time_new_from_iso8601 (ts, NULL) : NULL;
  if (!t)
    return g_strdup (*ts ? ts : "Unknown time");
  g_autoptr (GDateTime) now = g_date_time_new_now_local ();
  gint64 s = g_date_time_difference (now, t) / G_TIME_SPAN_SECOND;
  if (s < 60)
    return g_strdup ("just now");
  if (s < 3600)
    return g_strdup_printf ("%d min ago", (int) (s / 60));
  if (s < 86400)
    return g_strdup_printf ("%d h ago", (int) (s / 3600));
  int days = (int) (s / 86400);
  if (days == 1)
    return g_strdup ("yesterday");
  if (days < 7)
    return g_strdup_printf ("%d days ago", days);
  return g_date_time_format (t, "%e %B");
}

static char *
when_absolute (const char *ts)
{
  g_autoptr (GDateTime) t = ts && *ts ? g_date_time_new_from_iso8601 (ts, NULL) : NULL;
  return t ? g_date_time_format (t, "%e %B %Y, %H:%M") : g_strdup (ts);
}

/* `latency_ms` is the decision log's; a bare `latency` is read as milliseconds too */
static gboolean
latency_of (JsonObject *e, char **text)
{
  const char *keys[] = { "latency_ms", "latency" };
  for (guint k = 0; k < G_N_ELEMENTS (keys); k++) {
    if (!json_object_has_member (e, keys[k]))
      continue;
    JsonNode *n = json_object_get_member (e, keys[k]);
    if (!JSON_NODE_HOLDS_VALUE (n))
      continue;
    GType type = json_node_get_value_type (n);
    if (type != G_TYPE_INT64 && type != G_TYPE_DOUBLE)
      continue;
    double ms = json_node_get_double (n);
    *text = ms >= 1000 ? g_strdup_printf ("%.1f s", ms / 1000) : g_strdup_printf ("%d ms", (int) ms);
    return TRUE;
  }
  return FALSE;
}

typedef struct {
  JsonObject *e;   /* the entry, kept with the page */
} Entry;

static void
entry_free (Entry *en)
{
  json_object_unref (en->e);
  g_free (en);
}

static void
detail_open (GtkWidget *row, gpointer data)
{
  JsonObject *e = g_object_get_data (G_OBJECT (row), "perm-entry");
  GtkWidget *c;
  AdwNavigationPage *page = ns_page_new ("Decision", &c);
  Entry *en = g_new0 (Entry, 1);
  en->e = json_object_ref (e);
  g_object_set_data_full (G_OBJECT (page), "perm-entry", en, (GDestroyNotify) entry_free);

  GtkWidget *g = ns_group (c, "Decision");
  ns_row_info (g, "What was asked", str_of (e, "sentence"));
  const char *why = str_of (e, "why");
  if (*why)
    ns_row_info (g, "Why", why);
  g_autofree char *by = g_strdup_printf ("%s • %s", who_of (e), verdict_of (e));
  ns_row_info (g, "Decided by", by);
  g_autofree char *when = when_absolute (str_of (e, "ts"));
  ns_row_info (g, "When", when);
  char *lat = NULL;
  if (latency_of (e, &lat))
    ns_row_info (g, "Took", lat);
  g_free (lat);

  if (json_object_has_member (e, "reminders") && json_object_get_int_member (e, "reminders") > 0) {
    g_autofree char *n = g_strdup_printf ("%d", (int) json_object_get_int_member (e, "reminders"));
    ns_row_info (g, "Reminders sent", n);
  }
  if (!g_strcmp0 (str_of (e, "reason_code"), "timeout"))
    ns_row_info (g, "Answer", "No answer: counted as a No");

  const char *detail = str_of (e, "detail");
  if (*detail) {
    g = ns_group (c, "Details");
    GtkWidget *l = gtk_label_new (detail);
    gtk_label_set_xalign (GTK_LABEL (l), 0);
    gtk_label_set_wrap (GTK_LABEL (l), TRUE);
    gtk_label_set_selectable (GTK_LABEL (l), TRUE);
    gtk_widget_add_css_class (l, "monospace");
    gtk_widget_add_css_class (l, "ns-sub");
    ns_row_widget (g, NULL, l);
  }
  ns_push (page);
}

static void
history_fill (JsonArray *list, const char *err, gpointer data)
{
  Perm *ak = data;
  ns_group_clear (ak->history);
  guint n = list ? json_array_get_length (list) : 0;
  for (guint i = 0; i < n; i++) {
    JsonObject *e = json_array_get_object_element (list, i);
    const char *who = who_of (e), *sentence = str_of (e, "sentence");
    g_autofree char *ago = when_of (str_of (e, "ts"));
    g_autofree char *sub = g_strdup_printf ("%s • %s • %s", who, verdict_of (e), ago);
    GtkWidget *row = ns_row (ak->history, who_icon (who), who_color (who),
                             *sentence ? sentence : "An action", sub);
    g_object_set_data_full (G_OBJECT (row), "perm-entry", json_object_ref (e), (GDestroyNotify) json_object_unref);
    ns_row_on_activate (row, detail_open, ak);
  }
  ns_group_set_visible (ak->history, n > 0);
  gtk_widget_set_visible (ak->history_none, n == 0);
  if (err && *err) {
    g_autofree char *t = g_strdup_printf ("Could not read the history: %s", err);
    say (ak, t, TRUE);
  }
}

/* ---- refresh ---- */

static void
status_fill (Perm *ak)
{
  gboolean ask = !g_strcmp0 (mode_of (ak), "ask");
  gtk_widget_set_opacity (ak->ask_mark, ask);
  gtk_widget_set_opacity (ak->auto_mark, ask ? 0 : 1);
  screen_fill (ak);
  gates_fill (ak);
  base_fill (ak);
  rules_fill (ak);
  classifier_fill (ak);
  reminders_fill (ak);
}

static void
status_done (JsonObject *root, const char *err, gpointer data)
{
  Perm *ak = data;
  if (err || !root) {
    g_autofree char *t = g_strdup_printf ("Could not read the permissions: %s", err ? err : "no answer");
    say (ak, t, TRUE);
    return;
  }
  if (ak->st)
    json_object_unref (ak->st);
  ak->st = json_object_ref (root);
  status_fill (ak);
}

static void
perm_refresh (Perm *ak)
{
  static const char *const status[] = { "status", "--json", NULL };
  static const char *const hist[] = { "history", "-n", "30", "--json", NULL };
  ns_mc_run_bin (G_OBJECT (ak->page), "minerva-perm", status, NULL, status_done, ak);
  ns_mc_run_list (G_OBJECT (ak->page), "minerva-perm", hist, history_fill, ak);
}

static void
perm_free (Perm *ak)
{
  if (ak->st)
    json_object_unref (ak->st);
  g_free (ak);
}

/* the page shown again (a question answered from a notification, another app came back): its lists again */
static void
page_showing (AdwNavigationPage *page, gpointer data)
{
  perm_refresh (data);
}

/* ---- the Security and privacy row: "Auto • 3 rules", read from the status ---- */

static void
row_status_done (JsonObject *root, const char *err, gpointer data)
{
  GtkWidget *row = data;
  if (err || !root) {
    ns_row_set_subtitle (row, "Could not read them");
    return;
  }
  JsonArray *rules = arr_of (root, "rules");
  guint n = rules ? json_array_get_length (rules) : 0;
  g_autofree char *t = g_strdup_printf ("%s • %u %s",
                                        g_strcmp0 (str_of (root, "mode"), "ask") ? "Auto" : "Ask every time", n,
                                        n == 1 ? "rule" : "rules");
  ns_row_set_subtitle (row, t);
}

void
ns_perm_add_row (GtkWidget *group)
{
  gboolean there = ns_mc_prog_available ("minerva-perm");
  g_autofree char *what = msg_assistant_fill ("What {assistant} may do without asking");
  GtkWidget *row = ns_row (group, "system-lock-screen-symbolic", "red", "Permissions", there ? what : "Not installed");
  ns_row_on_activate (row, ns_page_permissions, NULL);
  if (there) {
    static const char *const args[] = { "status", "--json", NULL };
    ns_mc_run_bin (G_OBJECT (row), "minerva-perm", args, NULL, row_status_done, row);
  }
}

/* ---- the page ---- */

void
ns_page_permissions (GtkWidget *row, gpointer data)
{
  GtkWidget *c;
  AdwNavigationPage *page = ns_page_new ("Permissions", &c);
  Perm *ak = g_new0 (Perm, 1);
  ak->page = page;
  g_object_set_data_full (G_OBJECT (page), "perm", ak, (GDestroyNotify) perm_free);

  ak->msg = ns_note (c, "");
  gtk_widget_set_visible (ak->msg, FALSE);
  gtk_widget_set_margin_top (ak->msg, 0);

  if (!ns_mc_prog_available ("minerva-perm")) {
    g_autofree char *none = msg_assistant_fill ("{assistant}'s permissions are not available on this phone: minerva-perm "
                                                "is not installed.");
    ns_note (c, none);
    ns_push (page);
    return;
  }

  GtkWidget *g = ns_group (c, NULL);
  ak->screen_row = ns_row_toggle (g, "input-mouse-symbolic", "orange", "", "");
  g_signal_connect (ns_row_get_switch (ak->screen_row), "notify::active", G_CALLBACK (screen_toggled), ak);
  ak->screen_note = ns_note (c, "");
  gtk_widget_set_visible (ak->screen_note, FALSE);

  ns_agents_card (page, c);  /* agents of other machines in the chats (ns-agents.c) */

  g = ns_group (c, "Mode");
  GtkWidget *r = ns_row (g, NULL, NULL, "Ask every time", "Every action asks you before it runs");
  ak->ask_mark = mark_suffix (r);
  ns_row_on_activate (r, ask_clicked, ak);
  r = ns_row (g, NULL, NULL, "Auto", "A light model decides from your last message; unclear requests ask you");
  ak->auto_mark = mark_suffix (r);
  ns_row_on_activate (r, auto_clicked, ak);
  ns_note (c, "Auto is the default. A rule you add is checked before the model, and the hard gates below before "
              "everything.");

  ak->gates = ns_group (c, "Always refused");
  g_autofree char *shipped = msg_assistant_fill ("Shipped with {assistant} and checked first: no rule, mode or model can "
                                                 "allow them.");
  ns_note (c, shipped);

  ak->base = ns_group (c, "Allowed without asking");
  g_autofree char *safe = msg_assistant_fill ("The safe things: ordinary files, {assistant}'s own phone tools, read-only "
                                              "shell commands.");
  ns_note (c, safe);

  ak->rules = ns_group (c, "Your rules");
  ak->rules_none = ns_note (c, "No rules yet. Without a rule, the mode and the classifier decide.");
  gtk_widget_set_visible (ak->rules_none, FALSE);

  ak->history = ns_group (c, "History");
  g_autofree char *nothing = msg_assistant_fill ("No decision yet. Every one {assistant} makes is recorded here.");
  ak->history_none = ns_note (c, nothing);
  gtk_widget_set_visible (ak->history_none, FALSE);
  ns_note (c, "Newest first, the last 30. Tap one for its sentence, its reason and the raw request.");

  g = ns_group (c, "Classifier");
  ak->model_row = ns_row (g, "system-search-symbolic", "violet", "Model", "");
  ns_row_on_activate (ak->model_row, model_clicked, ak);
  ak->fallbacks = ns_group (c, "Fallbacks");
  g_autofree char *classifier = msg_assistant_fill ("The classifier is only used in Auto. When it fails for any reason, "
                                                    "{assistant} asks you. The fallbacks are tried in this order; change "
                                                    "them on the {assistant} page.");
  ns_note (c, classifier);

  g = ns_group (c, "Reminders");
  ak->rem_count = ns_row_value (g, "Reminders", "", FALSE, rem_count_entered, ak);
  ak->rem_interval = ns_row_value (g, "Between reminders (s)", "", FALSE, rem_interval_entered, ak);
  ak->rem_final = ns_row_value (g, "Last wait (s)", "", FALSE, rem_final_entered, ak);
  ns_note (c, "A question nobody answers is asked again, not refused: this many reminders (notification, a short line "
              "in Messages, and by voice only while the screen is on), this many seconds apart; after the last one "
              "and the last wait it counts as a No.");

  g_signal_connect (page, "showing", G_CALLBACK (page_showing), ak);
  perm_refresh (ak);
  ns_push (page);
}
