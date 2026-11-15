/* The Minerva page: which model the phone's agent thinks with, its fallbacks, per-role models, voice, budgets, and
 * whether the keys are there. Everything is read and written through `minerva-config` (Minerva's repository,
 * config/): `status --json` to read, `set key=value --apply --json` to write. The page keeps no state of its own,
 * and never shows a key: it takes one in a password dialog, hands it to `minerva-config key set` on stdin (never in
 * argv) and shows only "set, …last4" or "missing". */
#include "ns-pages.h"
#include "ns-mc.h"
#include "ns-ui.h"

#include <json-glib/json-glib.h>

#define str_of ns_json_str
#define obj_of ns_json_obj
#define arr_of ns_json_arr

typedef struct Mn Mn;

struct Mn {
  AdwNavigationPage *page;
  GtkWidget *msg;                       /* the last result: saved, restarted, or the error */
  GtkWidget *brain_g, *roles_g, *voice_g; /* filled once, with the first status (the pickers need the model list) */
  GtkWidget *chain, *stt, *notes;       /* rebuilt on every refresh (no pickers inside) */
  GtkWidget *stt_timeout;               /* the cloud recognizers' time limit row */
  GtkWidget *voice_id, *voice_cap, *budget_g, *spent; /* rows updated in place */
  GtkWidget *events, *bypass;           /* the events daemon's day: tokens used, and today's bypass switch */
  GtkWidget *sg_box;                    /* the data-driven groups (setting_groups of the status) go in here */
  GtkWidget *vp_info;                   /* the voiceprint's state row (a group's "voiceprint" data) */
  char      *vp_text[3];                /* its texts: enrolled, none, enrolling ({got}, {need}) */
  GPtrArray *vp_actions;                /* the voiceprint buttons: "mn-when" says in which state each shows */
  guint      poll_id;                   /* refreshes while an enrollment runs */
  guint      poll_s;
  GPtrArray *srows;                     /* their rows: "mn-key", "mn-kind" (and "mn-ids" for a picker) */
  GPtrArray *caps;                      /* the budget rows: each has its brain.conf key in "mn-key" */
  char      *cap_key;                   /* the budget in money: what the spent line is compared with */
  JsonObject *st;                       /* the last status */
  GStrv      chain_ids;                 /* the configured fallbacks, in order */
  GStrv      stt_ids;                   /* the speech recognizers before Vosk, in order (Vosk is always last) */
  char      *claude_hint;               /* "signed in"…: asked of the Claude CLI once, when the page opens */
  gboolean   picks_built;               /* the pickers are built once: a refresh must not free what a picker page points to */
  gboolean   busy;
  gboolean   groups_only;               /* the Prompt page: just the data-driven groups and the editors */
};

/* ---- running minerva-config (ns-mc.c) ---- */

typedef void (*MnDone) (Mn *m, JsonObject *root, const char *err, gpointer data);

typedef struct {
  Mn      *m;
  MnDone   done;
  gpointer data;
} Job;

static void
job_done (JsonObject *root, const char *err, gpointer data)
{
  Job *j = data;
  j->done (j->m, root, err, j->data);
  g_free (j);
}

/* `stdin_text` (or NULL) goes to the program's standard input: how a key travels, so it is in no process list */
static void
mn_run_in (Mn *m, const char *const *args, const char *stdin_text, MnDone done, gpointer data)
{
  Job *j = g_new0 (Job, 1);
  j->m = m;
  j->done = done;
  j->data = data;
  ns_mc_run (G_OBJECT (m->page), args, stdin_text, job_done, j);
}

static void
mn_run (Mn *m, const char *const *args, MnDone done, gpointer data)
{
  mn_run_in (m, args, NULL, done, data);
}

static void
say (Mn *m, const char *text, gboolean error)
{
  gtk_label_set_text (GTK_LABEL (m->msg), text);
  gtk_widget_set_visible (m->msg, text && *text);
  if (error)
    gtk_widget_add_css_class (m->msg, "ns-danger");
  else
    gtk_widget_remove_css_class (m->msg, "ns-danger");
}

/* ---- reading the status ---- */

static const char *
setting (Mn *m, const char *key)
{
  return str_of (obj_of (m->st, "brain"), key);
}

/* the catalog entry of a model id, or NULL */
static JsonObject *
model_of (Mn *m, const char *id)
{
  JsonArray *a = arr_of (m->st, "models");
  for (guint i = 0; a && i < json_array_get_length (a); i++) {
    JsonObject *o = json_array_get_object_element (a, i);
    if (!g_strcmp0 (str_of (o, "id"), id))
      return o;
  }
  return NULL;
}

static char *
model_label (JsonObject *o)
{
  const char *name = str_of (o, "name");
  g_autofree char *base = *name ? g_strdup (name) : g_strdup (str_of (o, "id"));
  gboolean vision = json_object_has_member (o, "vision") && json_object_get_boolean_member (o, "vision");
  gboolean free = json_object_has_member (o, "free") && json_object_get_boolean_member (o, "free");
  if (g_strcmp0 (str_of (o, "provider"), "opencode"))   /* the provider tells equal names apart */
    return g_strdup_printf ("%s (%s)%s%s", base, str_of (o, "provider"), vision ? " • sees images" : "",
                            free ? " • free" : "");
  return g_strdup_printf ("%s%s%s", base, vision ? " • sees images" : "", free ? " • free" : "");
}

/* a label for an id that may be missing from the catalog */
static char *
id_label (Mn *m, const char *id)
{
  JsonObject *o = model_of (m, id);
  return o ? model_label (o) : g_strdup_printf ("%s • not listed", id);
}

/* ---- writing ---- */

static void mn_refresh (Mn *m);
static void build_rows (Mn *m);

static void
set_done (Mn *m, JsonObject *root, const char *err, gpointer data)
{
  m->busy = FALSE;
  if (err) {
    g_autofree char *t = g_strdup_printf ("Not saved: %s", err);
    say (m, t, TRUE);
  } else {
    JsonArray *restarted = arr_of (root, "restarted");
    g_autoptr (GString) s = g_string_new (data ? "Saved." : "Saved and applied.");
    for (guint i = 0; restarted && i < json_array_get_length (restarted); i++)
      g_string_append_printf (s, "%s%s", i ? ", " : " Restarted: ", json_array_get_string_element (restarted, i));
    say (m, s->str, FALSE);
  }
  mn_refresh (m);
}

static void
mn_set_ex (Mn *m, const char *key, const char *value, gboolean apply)
{
  if (m->busy) {
    say (m, "Still applying the last change…", FALSE);
    return;
  }
  m->busy = TRUE;
  say (m, "Applying…", FALSE);
  g_autofree char *pair = g_strdup_printf ("%s=%s", key, value);
  const char *const with[] = { "set", "--apply", "--json", "--quiet", pair, NULL };
  const char *const without[] = { "set", "--json", "--quiet", pair, NULL };
  mn_run (m, apply ? with : without, set_done, apply ? NULL : GINT_TO_POINTER (1));
}

static void
mn_set (Mn *m, const char *key, const char *value)
{
  mn_set_ex (m, key, value, TRUE);
}

/* ---- pickers (built once) ---- */

typedef struct {
  Mn    *m;
  char  *key;
  GStrv  ids;
} Choice;

static void
choice_free (Choice *c)
{
  g_free (c->key);
  g_strfreev (c->ids);
  g_free (c);
}

static void
choice_picked (GtkWidget *row, guint index, gpointer data)
{
  Choice *c = data;
  if (index < g_strv_length (c->ids))
    mn_set (c->m, c->key, c->ids[index]);
}

/* A picker for `key`: the catalog (only the models that see images when `vision`), with "Same as main" first when
 * `default_label`. A current value the catalog does not list stays selectable as "… • not listed". */
static GtkWidget *
choice_row (Mn *m, GtkWidget *group, const char *title, const char *key, const char *default_label, gboolean vision)
{
  const char *current = setting (m, key);
  g_autoptr (GPtrArray) ids = g_ptr_array_new_with_free_func (g_free);
  g_autoptr (GPtrArray) labels = g_ptr_array_new_with_free_func (g_free);
  if (default_label) {
    g_ptr_array_add (ids, g_strdup (""));
    g_ptr_array_add (labels, g_strdup (default_label));
  }
  JsonArray *a = arr_of (m->st, "models");
  gboolean seen = FALSE;
  for (guint i = 0; a && i < json_array_get_length (a); i++) {
    JsonObject *o = json_array_get_object_element (a, i);
    if (vision && !(json_object_has_member (o, "vision") && json_object_get_boolean_member (o, "vision")))
      continue;
    g_ptr_array_add (ids, g_strdup (str_of (o, "id")));
    g_ptr_array_add (labels, model_label (o));
    seen = seen || !g_strcmp0 (str_of (o, "id"), current);
  }
  if (*current && !seen) {
    g_ptr_array_add (ids, g_strdup (current));
    g_ptr_array_add (labels, id_label (m, current));
  }
  g_ptr_array_add (ids, NULL);
  g_ptr_array_add (labels, NULL);
  guint selected = default_label ? 0 : G_MAXUINT;
  for (guint i = 0; ids->pdata[i]; i++)
    if (!g_strcmp0 (ids->pdata[i], current) && (*current || !default_label))
      selected = i;
  Choice *c = g_new0 (Choice, 1);
  c->m = m;
  c->key = g_strdup (key);
  c->ids = g_strdupv ((char **) ids->pdata);
  GtkWidget *row = ns_row_pick (group, title, (const char *const *) labels->pdata, selected, choice_picked, c);
  g_object_set_data_full (G_OBJECT (row), "mn-choice", c, (GDestroyNotify) choice_free);
  return row;
}

static void
engine_picked (GtkWidget *row, guint index, gpointer data)
{
  Mn *m = data;
  JsonArray *a = arr_of (m->st, "voice_engines");
  if (a && index < json_array_get_length (a))
    mn_set (m, "voice.engine", str_of (json_array_get_object_element (a, index), "id"));
}

typedef struct {
  Mn   *m;
  char *key;
} Field;

static void
field_entered (const char *text, gpointer data)
{
  Field *f = data;
  if (text)
    mn_set (f->m, f->key, text);
}

static void
field_free (Field *f)
{
  g_free (f->key);
  g_free (f);
}

static GtkWidget *
field_row (Mn *m, GtkWidget *group, const char *title, const char *key)
{
  Field *f = g_new0 (Field, 1);
  f->m = m;
  f->key = g_strdup (key);
  GtkWidget *row = ns_row_value (group, title, "", FALSE, field_entered, f);
  g_object_set_data_full (G_OBJECT (row), "mn-field", f, (GDestroyNotify) field_free);
  return row;
}

/* Today's events bypass: the daemon owns it (its state file), the page only asks `minerva-config events-bypass`,
 * which talks to it over D-Bus. Setting it back relocks the day. */
static void
bypass_done (Mn *m, JsonObject *root, const char *err, gpointer data)
{
  m->busy = FALSE;
  if (err) {
    g_autofree char *t = g_strdup_printf ("Not saved: %s", err);
    say (m, t, TRUE);
  }
  mn_refresh (m);
}

static void
bypass_toggled (GtkWidget *sw, GParamSpec *pspec, gpointer data)
{
  Mn *m = data;
  if (ns_switch_syncing (sw) || m->busy)
    return;
  m->busy = TRUE;
  say (m, "Applying…", FALSE);
  const char *const args[] = { "events-bypass", gtk_switch_get_active (GTK_SWITCH (sw)) ? "on" : "off",
                              "--json", NULL };
  mn_run (m, args, bypass_done, NULL);
}

/* ---- the fallback chain ---- */

/* an ordered list in brain.conf (the fallback chain, the speech recognizers): `ids` is the list as shown, `key` where
 * it is saved. A row's buttons carry the index of its entry. */
typedef struct {
  Mn         *m;
  guint       index;
  const char *key;
  GStrv      *ids;
} ChainOp;

static void
chain_save (Mn *m, const char *key, GStrv ids)
{
  g_autofree char *joined = g_strjoinv (", ", ids);
  mn_set (m, key, joined);
}

/* "a, b c" -> {"a", "b", "c"}; `skip` (or NULL) is left out */
static GStrv
split_ids (const char *text, const char *skip)
{
  g_auto (GStrv) raw = g_strsplit_set (text, ", ", -1);
  GPtrArray *out = g_ptr_array_new ();
  for (guint i = 0; raw[i]; i++)
    if (*raw[i] && g_strcmp0 (raw[i], skip))
      g_ptr_array_add (out, g_strdup (raw[i]));
  g_ptr_array_add (out, NULL);
  return (GStrv) g_ptr_array_free (out, FALSE);
}

static void
chain_move (GtkButton *b, gpointer data)
{
  ChainOp *op = data;
  Mn *m = op->m;
  int by = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (b), "mn-by"));
  guint n = g_strv_length (*op->ids);
  int to = (int) op->index + by;
  if (to < 0 || to >= (int) n)
    return;
  g_auto (GStrv) ids = g_strdupv (*op->ids);
  char *t = ids[op->index];
  ids[op->index] = ids[to];
  ids[to] = t;
  chain_save (m, op->key, ids);
}

static void
chain_remove (GtkButton *b, gpointer data)
{
  ChainOp *op = data;
  Mn *m = op->m;
  g_autoptr (GPtrArray) keep = g_ptr_array_new ();
  for (guint i = 0; (*op->ids)[i]; i++)
    if (i != op->index)
      g_ptr_array_add (keep, (*op->ids)[i]);
  g_ptr_array_add (keep, NULL);
  chain_save (m, op->key, (GStrv) keep->pdata);
}

static GtkWidget *
icon_button (const char *icon, const char *tip, GCallback cb, gpointer data, int by, gboolean sensitive)
{
  GtkWidget *b = gtk_button_new_from_icon_name (icon);
  gtk_widget_add_css_class (b, "flat");
  gtk_widget_add_css_class (b, "circular");
  gtk_widget_set_tooltip_text (b, tip);
  gtk_widget_set_sensitive (b, sensitive);
  g_object_set_data (G_OBJECT (b), "mn-by", GINT_TO_POINTER (by));
  g_signal_connect (b, "clicked", cb, data);
  return b;
}

static void
add_picked (GtkWidget *row, gpointer data)
{
  Mn *m = data;
  const char *id = g_object_get_data (G_OBJECT (row), "mn-id");
  g_auto (GStrv) ids = NULL;
  GPtrArray *a = g_ptr_array_new ();
  for (guint i = 0; m->chain_ids && m->chain_ids[i]; i++)
    g_ptr_array_add (a, m->chain_ids[i]);
  g_ptr_array_add (a, (char *) id);
  g_ptr_array_add (a, NULL);
  g_autofree char *joined = g_strjoinv (", ", (GStrv) a->pdata);
  g_ptr_array_free (a, TRUE);
  mn_set (m, "fallback.chain", joined);
  gtk_widget_activate_action (row, "navigation.pop", NULL);   /* back to Minerva */
}

static void
chain_add (GtkWidget *row, gpointer data)
{
  Mn *m = data;
  GtkWidget *c;
  AdwNavigationPage *page = ns_page_new ("Add a fallback", &c);
  GtkWidget *g = ns_group (c, NULL);
  JsonArray *a = arr_of (m->st, "models");
  guint shown = 0;
  for (guint i = 0; a && i < json_array_get_length (a); i++) {
    JsonObject *o = json_array_get_object_element (a, i);
    const char *id = str_of (o, "id");
    if (!g_strcmp0 (id, setting (m, "brain.main")) || g_strv_contains ((const char *const *) m->chain_ids, id))
      continue;
    g_autofree char *label = model_label (o);
    GtkWidget *r = ns_row (g, NULL, NULL, label, id);
    g_object_set_data_full (G_OBJECT (r), "mn-id", g_strdup (id), g_free);
    ns_row_on_activate (r, add_picked, m);
    shown++;
  }
  if (!shown)
    ns_note (c, "Every model opencode offers is already the main model or in the chain.");
  ns_push (page);
}

static void
chain_free_op (ChainOp *op)
{
  g_free (op);
}

static void
chain_fill (Mn *m)
{
  ns_group_clear (m->chain);
  g_strfreev (m->chain_ids);
  m->chain_ids = split_ids (setting (m, "fallback.chain"), NULL);
  guint n = g_strv_length (m->chain_ids);
  for (guint i = 0; i < n; i++) {
    JsonObject *o = model_of (m, m->chain_ids[i]);
    const char *name = o ? str_of (o, "name") : "";
    g_autofree char *title = g_strdup (*name ? name : m->chain_ids[i]);
    gboolean vision = o && json_object_has_member (o, "vision") && json_object_get_boolean_member (o, "vision");
    g_autofree char *sub = g_strdup_printf ("%s%s%s", m->chain_ids[i], vision ? " • sees images" : "",
                                            o ? "" : " • not listed");
    GtkWidget *row = ns_row (m->chain, NULL, NULL, title, sub);
    ChainOp *op = g_new0 (ChainOp, 1);
    op->m = m;
    op->index = i;
    op->key = "fallback.chain";
    op->ids = &m->chain_ids;
    g_object_set_data_full (G_OBJECT (row), "mn-op", op, (GDestroyNotify) chain_free_op);
    GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_append (GTK_BOX (box), icon_button ("go-up-symbolic", "Earlier", G_CALLBACK (chain_move), op, -1, i > 0));
    gtk_box_append (GTK_BOX (box), icon_button ("go-down-symbolic", "Later", G_CALLBACK (chain_move), op, 1, i + 1 < n));
    gtk_box_append (GTK_BOX (box), icon_button ("list-remove-symbolic", "Remove", G_CALLBACK (chain_remove), op, 0, TRUE));
    ns_row_suffix (row, box);
  }
  GtkWidget *add = ns_row_button (m->chain, "Add a model…", FALSE, chain_add, m);
  (void) add;
}

/* ---- speech recognition: the engines tried in order, Vosk (on the phone) always last ---- */

/* the recognizers a chain can name besides the fixed one come from minerva-config (status: stt_engines): Claude and
 * every stt-capable API provider of the registry */
static JsonObject *
stt_engine (Mn *m, const char *id)
{
  JsonArray *a = arr_of (m->st, "stt_engines");
  for (guint i = 0; a && i < json_array_get_length (a); i++) {
    JsonObject *o = json_array_get_object_element (a, i);
    if (!g_strcmp0 (str_of (o, "id"), id))
      return o;
  }
  return NULL;
}

static guint
stt_engine_count (Mn *m)
{
  JsonArray *a = arr_of (m->st, "stt_engines");
  return a ? json_array_get_length (a) : 0;
}

static const char *
stt_label (Mn *m, const char *id)
{
  const char *n = str_of (stt_engine (m, id), "name");
  return *n ? n : id;
}

/* what to say under an engine: Claude's login (asked of the CLI), a key present or missing */
static const char *
stt_hint (Mn *m, const char *id)
{
  if (!g_strcmp0 (id, "claude"))
    return m->claude_hint ? m->claude_hint : "checking…";
  const char *h = str_of (obj_of (m->st, "stt_hints"), id);
  return *h ? h : "unknown";
}

static void
stt_add_picked (GtkWidget *row, gpointer data)
{
  Mn *m = data;
  const char *id = g_object_get_data (G_OBJECT (row), "mn-id");
  g_autoptr (GPtrArray) a = g_ptr_array_new ();
  for (guint i = 0; m->stt_ids && m->stt_ids[i]; i++)
    g_ptr_array_add (a, m->stt_ids[i]);
  g_ptr_array_add (a, (char *) id);
  g_ptr_array_add (a, NULL);
  chain_save (m, "voice.stt_chain", (GStrv) a->pdata);
  gtk_widget_activate_action (row, "navigation.pop", NULL);   /* back to Minerva */
}

static void
stt_add (GtkWidget *row, gpointer data)
{
  Mn *m = data;
  GtkWidget *c;
  AdwNavigationPage *page = ns_page_new ("Add a recognizer", &c);
  GtkWidget *g = ns_group (c, NULL);
  JsonArray *a = arr_of (m->st, "stt_engines");
  guint shown = 0;
  for (guint i = 0; a && i < json_array_get_length (a); i++) {
    const char *id = str_of (json_array_get_object_element (a, i), "id");
    if (g_strv_contains ((const char *const *) m->stt_ids, id))
      continue;
    GtkWidget *r = ns_row (g, NULL, NULL, stt_label (m, id), stt_hint (m, id));
    g_object_set_data_full (G_OBJECT (r), "mn-id", g_strdup (id), g_free);
    ns_row_on_activate (r, stt_add_picked, m);
    shown++;
  }
  if (!shown)
    ns_note (c, "Every recognizer is already in the list.");
  ns_push (page);
}

static void
stt_fill (Mn *m)
{
  ns_group_clear (m->stt);
  g_strfreev (m->stt_ids);
  JsonObject *fixed = obj_of (m->st, "stt_fixed");   /* the local one, always last */
  m->stt_ids = split_ids (setting (m, "voice.stt_chain"), str_of (fixed, "id"));
  guint n = g_strv_length (m->stt_ids);
  for (guint i = 0; i < n; i++) {
    GtkWidget *row = ns_row (m->stt, NULL, NULL, stt_label (m, m->stt_ids[i]), stt_hint (m, m->stt_ids[i]));
    ChainOp *op = g_new0 (ChainOp, 1);
    op->m = m;
    op->index = i;
    op->key = "voice.stt_chain";
    op->ids = &m->stt_ids;
    g_object_set_data_full (G_OBJECT (row), "mn-op", op, (GDestroyNotify) chain_free_op);
    GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_append (GTK_BOX (box), icon_button ("go-up-symbolic", "Earlier", G_CALLBACK (chain_move), op, -1, i > 0));
    gtk_box_append (GTK_BOX (box), icon_button ("go-down-symbolic", "Later", G_CALLBACK (chain_move), op, 1, i + 1 < n));
    gtk_box_append (GTK_BOX (box), icon_button ("list-remove-symbolic", "Remove", G_CALLBACK (chain_remove), op, 0, TRUE));
    ns_row_suffix (row, box);
  }
  g_autofree char *last = g_strdup_printf ("%s • always the last one", str_of (fixed, "hint"));
  ns_row (m->stt, NULL, NULL, str_of (fixed, "name"), last);
  if (n < stt_engine_count (m))
    ns_row_button (m->stt, "Add a recognizer…", FALSE, stt_add, m);
}

static void
stt_status_done (Mn *m, JsonObject *root, const char *err, gpointer data)
{
  g_free (m->claude_hint);
  m->claude_hint = g_strdup (!err && root ? str_of (root, "claude") : "unknown");
  if (m->st)
    stt_fill (m);
}

/* ---- refresh ---- */

static void
note (Mn *m, const char *text)
{
  ns_note (m->notes, text);
}

static void
notes_fill (Mn *m)
{
  GtkWidget *c;
  while ((c = gtk_widget_get_first_child (m->notes)))
    gtk_box_remove (GTK_BOX (m->notes), c);
  JsonObject *eff = obj_of (m->st, "effective");
  const char *in_use = str_of (eff, "main");
  if (*in_use && g_strcmp0 (in_use, setting (m, "brain.main"))) {
    g_autofree char *t = g_strdup_printf ("In use now: %s", in_use);
    note (m, t);
  }
  JsonArray *notes = arr_of (m->st, "notes");
  for (guint i = 0; notes && i < json_array_get_length (notes); i++)
    note (m, json_array_get_string_element (notes, i));
  JsonObject *oc = obj_of (m->st, "opencode");
  if (oc && json_object_has_member (oc, "reachable") && !json_object_get_boolean_member (oc, "reachable"))
    note (m, "The agent server did not answer: the model list is empty.");
}

static void
fill_value (GtkWidget *row, const char *value)
{
  ns_row_value_set (row, value);
}

/* ---- the data-driven groups ----
 * `setting_groups` of the status (minerva-config, settings_ui.json) lists titled groups of rows: a key of brain.conf
 * with its label, kind (bool, choice, text, int, float), choices, and help. The page keeps no list of settings of its
 * own: a setting is a line of data there. */

static void
sw_toggled (GtkWidget *sw, GParamSpec *pspec, gpointer data)
{
  Mn *m = data;
  if (ns_switch_syncing (sw) || m->busy)
    return;
  mn_set (m, g_object_get_data (G_OBJECT (sw), "mn-key"), gtk_switch_get_active (GTK_SWITCH (sw)) ? "true" : "false");
}

/* ---- the owner's voiceprint (speaker check of the wake word): a state row and its buttons, all texts from the data ---- */

typedef struct {
  Mn    *m;
  GStrv  args;
} VpAction;

static void
vp_action_done (Mn *m, JsonObject *root, const char *err, gpointer data)
{
  if (err) {
    g_autofree char *t = g_strdup_printf ("%s", err);
    say (m, t, TRUE);
  } else {
    say (m, "", FALSE);
  }
  mn_refresh (m);
}

static void
vp_action_run (GtkWidget *row, gpointer data)
{
  VpAction *a = data;
  mn_run (a->m, (const char *const *) a->args, vp_action_done, NULL);
}

static void
vp_action_free (VpAction *a)
{
  g_strfreev (a->args);
  g_free (a);
}

static void
build_voiceprint (Mn *m, GtkWidget *grp, JsonObject *vp)
{
  const char *keys[3] = { "enrolled", "none", "enrolling" };
  for (int i = 0; i < 3; i++) {
    g_free (m->vp_text[i]);
    m->vp_text[i] = g_strdup (str_of (vp, keys[i]));
  }
  m->poll_s = (guint) json_object_get_int_member_with_default (vp, "poll_s", 2);
  m->vp_info = ns_row_info (grp, str_of (vp, "label"), "");
  JsonArray *acts = arr_of (vp, "actions");
  for (guint i = 0; acts && i < json_array_get_length (acts); i++) {
    JsonObject *o = json_array_get_object_element (acts, i);
    JsonArray *av = arr_of (o, "args");
    g_autoptr (GPtrArray) v = g_ptr_array_new ();
    for (guint k = 0; av && k < json_array_get_length (av); k++)
      g_ptr_array_add (v, g_strdup (json_array_get_string_element (av, k)));
    g_ptr_array_add (v, NULL);
    VpAction *a = g_new0 (VpAction, 1);
    a->m = m;
    a->args = g_strdupv ((char **) v->pdata);
    GtkWidget *row = ns_row_button (grp, str_of (o, "label"), json_object_get_boolean_member_with_default (o, "destructive", FALSE),
                                    vp_action_run, a);
    g_object_set_data_full (G_OBJECT (row), "mn-vp-action", a, (GDestroyNotify) vp_action_free);
    g_object_set_data_full (G_OBJECT (row), "mn-when", g_strdup (str_of (o, "when")), g_free);
    g_ptr_array_add (m->vp_actions, row);
  }
}

static gboolean
poll_tick (gpointer data)
{
  Mn *m = data;
  JsonObject *vp = obj_of (m->st, "voiceprint");
  if (!vp || !json_object_get_boolean_member_with_default (vp, "enrolling", FALSE)) {
    m->poll_id = 0;
    return G_SOURCE_REMOVE;
  }
  mn_refresh (m);
  return G_SOURCE_CONTINUE;
}

static void
fill_voiceprint (Mn *m)
{
  JsonObject *vp = obj_of (m->st, "voiceprint");
  if (!vp || !m->vp_info)
    return;
  gboolean enrolled = json_object_get_boolean_member_with_default (vp, "enrolled", FALSE);
  gboolean enrolling = json_object_get_boolean_member_with_default (vp, "enrolling", FALSE);
  g_autoptr (GString) t = g_string_new (m->vp_text[enrolling ? 2 : enrolled ? 0 : 1]);
  g_autofree char *got = g_strdup_printf ("%d", (int) json_object_get_int_member_with_default (vp, "got", 0));
  g_autofree char *need = g_strdup_printf ("%d", (int) json_object_get_int_member_with_default (vp, "need", 0));
  g_string_replace (t, "{got}", got, 0);
  g_string_replace (t, "{need}", need, 0);
  ns_row_set_subtitle (m->vp_info, t->str);
  for (guint i = 0; i < m->vp_actions->len; i++) {
    GtkWidget *row = m->vp_actions->pdata[i];
    const char *when = g_object_get_data (G_OBJECT (row), "mn-when");
    /* when: "" always, "idle" while no enrollment runs, "enrolled" with a voiceprint and none running */
    gboolean show = !*when || (!g_strcmp0 (when, "idle") && !enrolling) || (!g_strcmp0 (when, "enrolled") && enrolled && !enrolling);
    gtk_widget_set_visible (row, show);
  }
  for (guint i = 0; i < m->srows->len; i++) {
    GtkWidget *row = m->srows->pdata[i];
    if (g_object_get_data (G_OBJECT (row), "mn-needs-vp"))
      gtk_widget_set_sensitive (row, enrolled);
  }
  if (enrolling && !m->poll_id)
    m->poll_id = g_timeout_add_seconds (m->poll_s, poll_tick, m);
}

static void
build_groups (Mn *m)
{
  JsonArray *gs = arr_of (m->st, "setting_groups");
  for (guint i = 0; gs && i < json_array_get_length (gs); i++) {
    JsonObject *g = json_array_get_object_element (gs, i);
    GtkWidget *grp = ns_group (m->sg_box, str_of (g, "title"));
    JsonArray *rows = arr_of (g, "rows");
    for (guint j = 0; rows && j < json_array_get_length (rows); j++) {
      JsonObject *r = json_array_get_object_element (rows, j);
      const char *key = str_of (r, "key"), *kind = str_of (r, "kind"), *label = str_of (r, "label");
      const char *help = str_of (r, "help");
      GtkWidget *row;
      if (!g_strcmp0 (kind, "bool")) {
        row = ns_row_switch (grp, label, *help ? help : NULL, NULL, NULL);
        GtkWidget *sw = ns_row_get_switch (row);
        g_object_set_data_full (G_OBJECT (sw), "mn-key", g_strdup (key), g_free);
        g_signal_connect (sw, "notify::active", G_CALLBACK (sw_toggled), m);
      } else if (!g_strcmp0 (kind, "choice")) {
        JsonArray *ch = arr_of (r, "choices");
        g_autoptr (GPtrArray) ids = g_ptr_array_new_with_free_func (g_free);
        g_autoptr (GPtrArray) labels = g_ptr_array_new_with_free_func (g_free);
        for (guint k = 0; ch && k < json_array_get_length (ch); k++) {
          JsonObject *o = json_array_get_object_element (ch, k);
          g_ptr_array_add (ids, g_strdup (str_of (o, "id")));
          g_ptr_array_add (labels, g_strdup (str_of (o, "label")));
        }
        g_ptr_array_add (ids, NULL);
        g_ptr_array_add (labels, NULL);
        Choice *c = g_new0 (Choice, 1);
        c->m = m;
        c->key = g_strdup (key);
        c->ids = g_strdupv ((char **) ids->pdata);
        row = ns_row_pick (grp, label, (const char *const *) labels->pdata, 0, choice_picked, c);
        g_object_set_data_full (G_OBJECT (row), "mn-choice", c, (GDestroyNotify) choice_free);
        if (*help)
          ns_row_set_subtitle (row, help);
      } else {
        row = field_row (m, grp, label, key);
        if (*help)
          ns_row_set_subtitle (row, help);
      }
      g_object_set_data_full (G_OBJECT (row), "mn-key", g_strdup (key), g_free);
      g_object_set_data_full (G_OBJECT (row), "mn-kind", g_strdup (kind), g_free);
      g_object_set_data (G_OBJECT (row), "mn-needs-vp", GINT_TO_POINTER (!g_strcmp0 (str_of (r, "needs"), "voiceprint")));
      g_ptr_array_add (m->srows, row);
    }
    JsonObject *vp = obj_of (g, "voiceprint");
    if (vp)
      build_voiceprint (m, grp, vp);
    const char *nt = str_of (g, "note");
    if (*nt)
      ns_note (m->sg_box, nt);
  }
}

static void
fill_groups (Mn *m)
{
  for (guint i = 0; i < m->srows->len; i++) {
    GtkWidget *row = m->srows->pdata[i];
    const char *key = g_object_get_data (G_OBJECT (row), "mn-key"), *kind = g_object_get_data (G_OBJECT (row), "mn-kind");
    const char *v = setting (m, key);
    if (!g_strcmp0 (kind, "bool")) {
      ns_switch_quiet (ns_row_get_switch (row), !g_strcmp0 (v, "true"));
    } else if (!g_strcmp0 (kind, "choice")) {
      Choice *c = g_object_get_data (G_OBJECT (row), "mn-choice");
      for (guint k = 0; c && c->ids[k]; k++)
        if (!g_strcmp0 (c->ids[k], v))
          ns_row_pick_select (row, k);
    } else {
      fill_value (row, v);
    }
  }
}

static void
update_fields (Mn *m)
{
  fill_value (m->voice_id, setting (m, "voice.voice"));
  fill_value (m->voice_cap, setting (m, "voice.daily_chars"));
  fill_value (m->stt_timeout, setting (m, "voice.stt_timeout_s"));
  for (guint i = 0; i < m->caps->len; i++)
    fill_value (m->caps->pdata[i], setting (m, g_object_get_data (m->caps->pdata[i], "mn-key")));
  JsonObject *ev = obj_of (m->st, "events");
  if (ev && m->events) {
    int used = (int) json_object_get_int_member (ev, "used");
    int budget = (int) json_object_get_int_member (ev, "budget");
    int online = (int) json_object_get_int_member (ev, "online_daily");
    int online_used = (int) json_object_get_int_member (ev, "online_used");
    g_autofree char *t = g_strdup_printf ("%d of %d tokens%s • face online %d/%d • %s", used, budget,
                                         json_object_get_boolean_member (ev, "bypass") ? " • bypassed" : "",
                                         online_used, online,
        json_object_get_boolean_member (ev, "running") ? "service running" : "service not running");
    ns_row_set_subtitle (m->events, t);
    if (m->bypass)
      ns_switch_quiet (ns_row_get_switch (m->bypass), json_object_get_boolean_member (ev, "bypass"));
  }
  JsonObject *b = obj_of (m->st, "budget");
  const char *cap = m->cap_key ? setting (m, m->cap_key) : "";
  if (b && json_object_has_member (b, "usage_usd") && !json_node_is_null (json_object_get_member (b, "usage_usd"))) {
    g_autofree char *t = g_strdup_printf ("$%.2f%s%s%s", json_object_get_double_member (b, "usage_usd"),
                                          *cap ? " of €" : "", cap,
                                          json_object_get_boolean_member (b, "over") ? " • over the cap" : "");
    ns_row_set_subtitle (m->spent, t);
  } else {
    ns_row_set_subtitle (m->spent, *cap && g_ascii_strtod (cap, NULL) > 0 ? "Not checked yet" : "No cap set: no paid use");
  }
}

static void
status_done (Mn *m, JsonObject *root, const char *err, gpointer data)
{
  if (err || !root) {
    g_autofree char *t = g_strdup_printf ("Could not read the settings: %s", err ? err : "no answer");
    say (m, t, TRUE);
    return;
  }
  if (m->st)
    json_object_unref (m->st);
  m->st = json_object_ref (root);
  if (m->groups_only) {
    if (!m->picks_built) {
      m->picks_built = TRUE;
      build_groups (m);
      ns_prompt_build (m->sg_box, G_OBJECT (m->page), obj_of (m->st, "prompt_ui"));
    }
    fill_groups (m);
    fill_voiceprint (m);
    return;
  }
  if (!m->picks_built)
    build_rows (m);
  chain_fill (m);
  stt_fill (m);
  notes_fill (m);
  update_fields (m);
  fill_groups (m);
  fill_voiceprint (m);
}

static void
mn_refresh (Mn *m)
{
  const char *const args[] = { "status", "--json", "--fast", NULL };
  mn_run (m, args, status_done, NULL);
}

/* ---- the page ---- */

static void
mn_free (Mn *m)
{
  if (m->st)
    json_object_unref (m->st);
  if (m->caps)
    g_ptr_array_free (m->caps, TRUE);
  if (m->srows)
    g_ptr_array_free (m->srows, TRUE);
  if (m->poll_id)
    g_source_remove (m->poll_id);
  if (m->vp_actions)
    g_ptr_array_free (m->vp_actions, TRUE);
  for (int i = 0; i < 3; i++)
    g_free (m->vp_text[i]);
  g_free (m->cap_key);
  g_strfreev (m->chain_ids);
  g_strfreev (m->stt_ids);
  g_free (m->claude_hint);
  g_free (m);
}

/* Minerva > Prompt (`--page prompt`): the wake word and reply groups come with the status (data), then the editors
 * (ns-prompt.c) */
void
ns_page_prompt (GtkWidget *row, gpointer data)
{
  GtkWidget *c;
  AdwNavigationPage *page = ns_page_new ("Prompt", &c);
  Mn *m = g_new0 (Mn, 1);
  m->page = page;
  m->groups_only = TRUE;
  g_object_set_data_full (G_OBJECT (page), "mn", m, (GDestroyNotify) mn_free);
  m->msg = ns_note (c, "");
  gtk_widget_set_visible (m->msg, FALSE);
  gtk_widget_set_margin_top (m->msg, 0);
  m->srows = g_ptr_array_new ();
  m->vp_actions = g_ptr_array_new ();
  m->sg_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_append (GTK_BOX (c), m->sg_box);
  mn_refresh (m);
  ns_push (page);
}

void
ns_page_minerva (GtkWidget *row, gpointer data)
{
  GtkWidget *c;
  AdwNavigationPage *page = ns_page_new ("Minerva", &c);
  Mn *m = g_new0 (Mn, 1);
  m->page = page;
  g_object_set_data_full (G_OBJECT (page), "mn", m, (GDestroyNotify) mn_free);

  m->msg = ns_note (c, "");
  gtk_widget_set_visible (m->msg, FALSE);
  gtk_widget_set_margin_top (m->msg, 0);

  m->brain_g = ns_group (c, "Brain");
  m->notes = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
  gtk_box_append (GTK_BOX (c), m->notes);

  m->chain = ns_group (c, "Fallback chain");
  ns_note (c, "Tried in this order when the main model cannot answer. A gateway's models fall back inside the "
              "gateway; free opencode models are the last resort when no paid provider is reachable.");

  m->roles_g = ns_group (c, "Models by role");
  ns_note (c, "Eyes and Mailbox are saved, but nothing reads them yet.");

  m->voice_g = ns_group (c, "Voice");

  m->srows = g_ptr_array_new ();
  m->vp_actions = g_ptr_array_new ();

  m->stt = ns_group (c, "Speech recognition");
  GtkWidget *sg = ns_group (c, NULL);
  m->stt_timeout = field_row (m, sg, "Cloud time limit (seconds)", "voice.stt_timeout_s");
  ns_note (c, "What you say after the wake word is sent to the first recognizer that is ready, in this order; one that "
              "fails or takes longer than the time limit (1 to 10 s) passes it to the next. Vosk runs on the phone and "
              "is always the last. The wake word itself is always recognized on the phone.");

  m->caps = g_ptr_array_new ();
  m->budget_g = ns_group (c, "Budgets");   /* its rows come with the status: one per provider with a budget */
  ns_note (c, "Empty or 0 means no paid use: the paid models stay off, and only the free ones answer, until you set a "
              "cap. Over the cap, Minerva falls back to the free models until the month turns. Checked hourly.");

  GtkWidget *pg = ns_group (c, NULL);   /* wake word, reply, system prompt, state template: the Prompt page */
  ns_row (pg, "format-justify-left-symbolic", "violet", "Prompt", "Wake word • Reply • System prompt • State template");
  ns_row_on_activate (gtk_widget_get_last_child (pg), ns_page_prompt, NULL);
  ns_people_add_entry (c, G_OBJECT (page));   /* Persone: its row and texts come from `people ui` */

  GtkWidget *kg = ns_group (c, NULL);
  ns_row (kg, NULL, NULL, "API keys", "Providers and their keys • Accounts");
  ns_row_on_activate (gtk_widget_get_last_child (kg), ns_page_api_keys, NULL);
  ns_note (c, "Keys for every provider, and endpoints of your own, are managed under Accounts. Without a key only the "
              "free opencode models are used.");

  say (m, "", FALSE);
  mn_refresh (m);
  const char *const stt_args[] = { "stt-status", "--json", NULL };
  mn_run (m, stt_args, stt_status_done, NULL);   /* the Claude CLI's login, a second or so: shown when it comes */
  ns_push (page);
}

/* the pickers and the voice card need the model list and the values, so they wait for the first status */
static void
build_rows (Mn *m)
{
  m->picks_built = TRUE;
  choice_row (m, m->brain_g, "Main model", "brain.main", NULL, FALSE);
  choice_row (m, m->roles_g, "Messages", "roles.chat", "Same as main", FALSE);
  choice_row (m, m->roles_g, "Events", "roles.events", "Same as main", FALSE);
  choice_row (m, m->roles_g, "Eyes (looks at pictures)", "roles.eyes", "Same as main", TRUE);
  choice_row (m, m->roles_g, "Mailbox (other agents)", "roles.mailbox", "Same as main", FALSE);
  JsonArray *ve = arr_of (m->st, "voice_engines");   /* the phone's own, then every provider with a voice */
  g_autoptr (GPtrArray) labels = g_ptr_array_new ();
  guint sel = 0;
  for (guint i = 0; ve && i < json_array_get_length (ve); i++) {
    JsonObject *o = json_array_get_object_element (ve, i);
    g_ptr_array_add (labels, (char *) str_of (o, "label"));
    if (!g_strcmp0 (str_of (o, "id"), setting (m, "voice.engine")))
      sel = i;
  }
  g_ptr_array_add (labels, NULL);
  ns_row_pick (m->voice_g, "Speech engine", (const char *const *) labels->pdata, sel, engine_picked, m);
  m->voice_id = field_row (m, m->voice_g, "Cloud voice name", "voice.voice");
  m->voice_cap = field_row (m, m->voice_g, "Characters per day", "voice.daily_chars");
  JsonArray *br = arr_of (m->st, "budget_rows");
  for (guint i = 0; br && i < json_array_get_length (br); i++) {
    JsonObject *o = json_array_get_object_element (br, i);
    GtkWidget *r = field_row (m, m->budget_g, str_of (o, "label"), str_of (o, "key"));
    g_object_set_data_full (G_OBJECT (r), "mn-key", g_strdup (str_of (o, "key")), g_free);
    g_ptr_array_add (m->caps, r);
    if (!m->cap_key && json_object_get_boolean_member (o, "money"))
      m->cap_key = g_strdup (str_of (o, "key"));
  }
  m->events = ns_row_info (m->budget_g, "Events today", "");
  m->bypass = ns_row_switch (m->budget_g, "Bypass the events limit today",
                             "The events keep reaching her until tomorrow, whatever the quota says", NULL, NULL);
  g_signal_connect (ns_row_get_switch (m->bypass), "notify::active", G_CALLBACK (bypass_toggled), m);
  m->spent = ns_row_info (m->budget_g, "Spent this month", "");
}

/* ---- the model list as a page of its own (Permissions > Classifier) ----
 * The same source as this page's pickers, `minerva-config models --json`, and the same labels; `picked` gets the id
 * of the row that was tapped. */

struct _ModelPick {
  char         *title, *current;
  NsModelPicked picked;
  gpointer      data;
};

typedef struct _ModelPick ModelPick;

static void
model_pick_free (ModelPick *p)
{
  g_free (p->title);
  g_free (p->current);
  g_free (p);
}

static void
model_row_tapped (GtkWidget *row, gpointer data)
{
  ModelPick *p = data;
  g_autofree char *id = g_strdup (g_object_get_data (G_OBJECT (row), "mp-id"));
  NsModelPicked picked = p->picked;
  gpointer into = p->data;   /* the page, and p with it, may go at once */
  gtk_widget_activate_action (row, "navigation.pop", NULL);
  if (picked)
    picked (id, into);
}

static void
model_pick_done (JsonArray *list, const char *err, gpointer data)
{
  ModelPick *p = data;
  GtkWidget *c;
  AdwNavigationPage *page = ns_page_new (p->title, &c);
  g_object_set_data_full (G_OBJECT (page), "mp", p, (GDestroyNotify) model_pick_free);
  GtkWidget *g = ns_group (c, NULL);
  guint n = 0;
  for (guint i = 0; list && i < json_array_get_length (list); i++) {
    JsonObject *o = json_array_get_object_element (list, i);
    const char *id = str_of (o, "id");
    g_autofree char *label = model_label (o);
    gboolean cur = !g_strcmp0 (id, p->current);
    GtkWidget *r = ns_row (g, NULL, NULL, label, cur ? "In use now" : NULL);
    g_object_set_data_full (G_OBJECT (r), "mp-id", g_strdup (id), g_free);
    if (cur) {
      GtkWidget *mark = gtk_image_new_from_icon_name ("object-select-symbolic");
      gtk_widget_add_css_class (mark, "ns-accent");
      ns_row_suffix (r, mark);
    }
    ns_row_on_activate (r, model_row_tapped, p);
    n++;
  }
  if (!n) {
    g_autofree char *t = err && *err ? g_strdup_printf ("Could not read the models: %s", err)
                                     : g_strdup ("No model is listed. Is the agent server running?");
    ns_note (c, t);
  } else {
    ns_note (c, "The models the phone's agent offers. Fallbacks stay as they are.");
  }
  ns_push (page);
}

void
ns_model_pick (GtkWidget *from, const char *title, const char *current, NsModelPicked picked, gpointer data)
{
  static const char *const args[] = { "models", "--json", NULL };
  ModelPick *p = g_new0 (ModelPick, 1);
  p->title = g_strdup (title);
  p->current = g_strdup (current);
  p->picked = picked;
  p->data = data;
  ns_mc_run_list (G_OBJECT (from), "minerva-config", args, model_pick_done, p);
}
