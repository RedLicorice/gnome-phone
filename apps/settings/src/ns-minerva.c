/* The Minerva page: which model the phone's agent thinks with, its fallbacks, per-role models, voice, budgets, and
 * whether the keys are there. Everything is read and written through `minerva-config` (Minerva's repository,
 * config/): `status --json` to read, `set key=value --apply --json` to write. The page keeps no state of its own,
 * and never shows a key: it takes one in a password dialog, hands it to `minerva-config key set` on stdin (never in
 * argv) and shows only "set, …last4" or "missing". */
#include "ns-pages.h"
#include "ns-ui.h"

#include <json-glib/json-glib.h>

typedef struct Mn Mn;

/* an API key: the name `minerva-config key` knows, the row that shows "set, …last4" and the Remove button */
typedef struct {
  Mn         *m;
  const char *name, *label, *file;
  GtkWidget  *row, *remove;
} KeyRow;

struct Mn {
  AdwNavigationPage *page;
  GtkWidget *msg;                       /* the last result: saved, restarted, or the error */
  GtkWidget *brain_g, *roles_g, *voice_g; /* filled once, with the first status (the pickers need the model list) */
  GtkWidget *chain, *notes;             /* rebuilt on every refresh (no pickers inside) */
  GtkWidget *voice_id, *voice_cap, *or_cap, *el_cap, *spent, *key_pw; /* rows updated in place */
  KeyRow     keys[2];                   /* OpenRouter, ElevenLabs */
  GtkWidget *bat_val, *bat_scale, *bat_state; /* Max charge: the value row, the slider, the charge service's state */
  guint      bat_timer;                 /* a drag is saved 600 ms after the last move */
  gboolean   bat_syncing;               /* the slider is being set from the status */
  JsonObject *st;                       /* the last status */
  GStrv      chain_ids;                 /* the configured fallbacks, in order */
  gboolean   picks_built;               /* the pickers are built once: a refresh must not free what a picker page points to */
  gboolean   busy;
};

static char *
mn_bin (void)
{
  const char *env = g_getenv ("MINERVA_CONFIG_BIN");
  return env && *env ? g_strdup (env)
                     : g_build_filename (g_get_home_dir (), ".local", "share", "minerva", "bin", "minerva-config", NULL);
}

gboolean
ns_minerva_available (void)
{
  g_autofree char *bin = mn_bin ();
  return g_file_test (bin, G_FILE_TEST_IS_EXECUTABLE);
}

/* ---- running minerva-config ---- */

typedef void (*MnDone) (Mn *m, JsonObject *root, const char *err, gpointer data);

typedef struct {
  Mn       *m;
  GObject  *page;                       /* a reference: the answer may come after the page is closed */
  MnDone    done;
  gpointer  data;
} Job;

static void
job_done (GObject *src, GAsyncResult *res, gpointer data)
{
  Job *j = data;
  g_autofree char *out = NULL, *err = NULL;
  g_autoptr (GError) e = NULL;
  g_subprocess_communicate_utf8_finish (G_SUBPROCESS (src), res, &out, &err, &e);
  gboolean ok = !e && g_subprocess_get_if_exited (G_SUBPROCESS (src)) &&
                g_subprocess_get_exit_status (G_SUBPROCESS (src)) == 0;
  g_autoptr (JsonParser) parser = json_parser_new ();
  JsonObject *root = NULL;
  if (ok && out && *out && json_parser_load_from_data (parser, out, -1, NULL)) {
    JsonNode *n = json_parser_get_root (parser);
    root = n && JSON_NODE_HOLDS_OBJECT (n) ? json_node_get_object (n) : NULL;
  }
  g_autofree char *why = NULL;
  if (!ok) {
    if (err && *err)
      why = g_strdup (g_strstrip (err));
    else
      why = g_strdup (e ? e->message : "minerva-config failed");
  }
  j->done (j->m, root, why, j->data);
  g_object_unref (j->page);
  g_free (j);
}

/* `stdin_text` (or NULL) goes to the program's standard input: how a key travels, so it is in no process list */
static void
mn_run_in (Mn *m, const char *const *args, const char *stdin_text, MnDone done, gpointer data)
{
  g_autofree char *bin = mn_bin ();
  g_autoptr (GPtrArray) argv = g_ptr_array_new ();
  g_ptr_array_add (argv, bin);
  for (guint i = 0; args[i]; i++)
    g_ptr_array_add (argv, (char *) args[i]);
  g_ptr_array_add (argv, NULL);
  g_autoptr (GError) err = NULL;
  GSubprocess *p = g_subprocess_newv ((const char *const *) argv->pdata,
                                      G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE |
                                      (stdin_text ? G_SUBPROCESS_FLAGS_STDIN_PIPE : G_SUBPROCESS_FLAGS_NONE), &err);
  if (!p) {
    done (m, NULL, err->message, data);
    return;
  }
  Job *j = g_new0 (Job, 1);
  j->m = m;
  j->page = g_object_ref (G_OBJECT (m->page));
  j->done = done;
  j->data = data;
  g_subprocess_communicate_utf8_async (p, stdin_text, NULL, job_done, j);
  g_object_unref (p);
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
str_of (JsonObject *o, const char *member)
{
  return o && json_object_has_member (o, member) && json_object_get_string_member (o, member)
           ? json_object_get_string_member (o, member) : "";
}

static JsonObject *
obj_of (JsonObject *o, const char *member)
{
  return o && json_object_has_member (o, member) && JSON_NODE_HOLDS_OBJECT (json_object_get_member (o, member))
           ? json_object_get_object_member (o, member) : NULL;
}

static JsonArray *
arr_of (JsonObject *o, const char *member)
{
  return o && json_object_has_member (o, member) && JSON_NODE_HOLDS_ARRAY (json_object_get_member (o, member))
           ? json_object_get_array_member (o, member) : NULL;
}

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
  mn_set (data, "voice.engine", index == 1 ? "elevenlabs" : "piper");
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

/* ---- the fallback chain ---- */

typedef struct {
  Mn   *m;
  guint index;
} ChainOp;

static void
chain_save (Mn *m, GStrv ids)
{
  g_autofree char *joined = g_strjoinv (", ", ids);
  mn_set (m, "fallback.chain", joined);
}

static void
chain_move (GtkButton *b, gpointer data)
{
  ChainOp *op = data;
  Mn *m = op->m;
  int by = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (b), "mn-by"));
  guint n = g_strv_length (m->chain_ids);
  int to = (int) op->index + by;
  if (to < 0 || to >= (int) n)
    return;
  g_auto (GStrv) ids = g_strdupv (m->chain_ids);
  char *t = ids[op->index];
  ids[op->index] = ids[to];
  ids[to] = t;
  chain_save (m, ids);
}

static void
chain_remove (GtkButton *b, gpointer data)
{
  ChainOp *op = data;
  Mn *m = op->m;
  g_autoptr (GPtrArray) keep = g_ptr_array_new ();
  for (guint i = 0; m->chain_ids[i]; i++)
    if (i != op->index)
      g_ptr_array_add (keep, m->chain_ids[i]);
  g_ptr_array_add (keep, NULL);
  chain_save (m, (GStrv) keep->pdata);
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
  m->chain_ids = g_strsplit_set (setting (m, "fallback.chain"), ", ", -1);
  /* g_strsplit_set leaves empty strings between a comma and a space: drop them */
  guint w = 0;
  for (guint r = 0; m->chain_ids[r]; r++) {
    if (*m->chain_ids[r])
      m->chain_ids[w++] = m->chain_ids[r];
    else
      g_free (m->chain_ids[r]);
  }
  m->chain_ids[w] = NULL;
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

/* ---- API keys ---- */

/* `key set|remove NAME --apply`: apply restarts the agent server when the OpenRouter key came or went */
static void
key_run (KeyRow *k, const char *action, const char *secret)
{
  Mn *m = k->m;
  if (m->busy) {
    say (m, "Still applying the last change…", FALSE);
    return;
  }
  m->busy = TRUE;
  say (m, "Applying…", FALSE);
  const char *const args[] = { "key", action, k->name, "--apply", "--json", NULL };
  mn_run_in (m, args, secret, set_done, NULL);
}

static void
key_entered (const char *text, gpointer data)
{
  KeyRow *k = data;
  if (!text || !*text)
    return;
  key_run (k, "set", text);   /* the dialog's string is not kept; minerva-config strips and checks it */
}

static void
key_activated (GtkWidget *row, gpointer data)
{
  KeyRow *k = data;
  g_autofree char *heading = g_strdup_printf ("%s key", k->label);
  g_autofree char *body = g_strdup_printf ("Paste the key. It is saved in ~/.config/minerva/%s, readable by you only, "
                                           "and never shown again.", k->file);
  ns_ask (row, heading, body, "", TRUE, "Save", key_entered, k);
}

static void
key_removed (gboolean yes, gpointer data)
{
  if (yes)
    key_run (data, "remove", NULL);
}

static void
key_remove_clicked (GtkButton *b, gpointer data)
{
  KeyRow *k = data;
  g_autofree char *heading = g_strdup_printf ("Remove the %s key?", k->label);
  ns_confirm (GTK_WIDGET (b), heading, "The file is deleted. You can enter a key again at any time.", "Remove", TRUE,
              key_removed, k);
}

static void
key_row_build (Mn *m, GtkWidget *group, KeyRow *k, const char *name, const char *label, const char *file)
{
  k->m = m;
  k->name = name;
  k->label = label;
  k->file = file;
  g_autofree char *title = g_strdup_printf ("%s key", label);
  k->row = ns_row (group, NULL, NULL, title, "…");
  ns_row_on_activate (k->row, key_activated, k);
  k->remove = icon_button ("user-trash-symbolic", "Remove the key", G_CALLBACK (key_remove_clicked), k, 0, FALSE);
  ns_row_suffix (k->row, k->remove);
}

static void
keys_update (Mn *m)
{
  JsonObject *hints = obj_of (m->st, "key_hints");
  for (guint i = 0; i < G_N_ELEMENTS (m->keys); i++) {
    const char *h = str_of (hints, m->keys[i].name);
    if (!*h)
      h = "missing";
    ns_row_set_subtitle (m->keys[i].row, h);
    gtk_widget_set_sensitive (m->keys[i].remove, g_str_has_prefix (h, "set"));
  }
}

/* ---- battery: Max charge ---- */

static int
bat_value (Mn *m)
{
  int v = (int) g_ascii_strtoll (setting (m, "battery.max_charge"), NULL, 10);
  return v < 50 || v > 100 ? 100 : v;
}

static void
bat_show (Mn *m, int v)
{
  g_autofree char *t = v >= 100 ? g_strdup ("Off (100 %)") : g_strdup_printf ("%d %%", v);
  ns_row_set_subtitle (m->bat_val, t);
}

static gboolean
bat_save (gpointer data)
{
  Mn *m = data;
  m->bat_timer = 0;
  g_autofree char *v = g_strdup_printf ("%d", (int) gtk_range_get_value (GTK_RANGE (m->bat_scale)));
  mn_set_ex (m, "battery.max_charge", v, FALSE);   /* nothing to render: the charge service polls brain.conf */
  return G_SOURCE_REMOVE;
}

static void
bat_moved (GtkRange *range, gpointer data)
{
  Mn *m = data;
  if (m->bat_syncing)
    return;
  double raw = gtk_range_get_value (range), v = 5 * (int) (raw / 5 + 0.5);   /* steps of 5 */
  if (v != raw) {
    m->bat_syncing = TRUE;
    gtk_range_set_value (range, v);
    m->bat_syncing = FALSE;
  }
  bat_show (m, (int) v);
  if (m->bat_timer)
    g_source_remove (m->bat_timer);
  m->bat_timer = g_timeout_add (600, bat_save, m);
}

static void
bat_update (Mn *m)
{
  if (!m->bat_timer) {   /* a drag in progress is not overwritten */
    int v = bat_value (m);
    m->bat_syncing = TRUE;
    gtk_range_set_value (GTK_RANGE (m->bat_scale), v);
    m->bat_syncing = FALSE;
    bat_show (m, v);
  }
  JsonObject *b = obj_of (m->st, "battery");   /* the charge service's state file; absent: nothing shown */
  const char *state = str_of (b, "state"), *soc = str_of (b, "soc_estimate"), *why = str_of (b, "reason");
  gtk_widget_set_visible (m->bat_state, *state != 0);
  if (*state) {
    g_autoptr (GString) t = g_string_new (state);
    t->str[0] = g_ascii_toupper (t->str[0]);
    if (*soc)
      g_string_append_printf (t, " • about %s %%", soc);
    if (*why)
      g_string_append_printf (t, " • %s", why);
    ns_row_set_subtitle (m->bat_state, t->str);
  }
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

static void
update_fields (Mn *m)
{
  fill_value (m->voice_id, setting (m, "voice.voice"));
  fill_value (m->voice_cap, setting (m, "voice.daily_chars"));
  fill_value (m->or_cap, setting (m, "budgets.openrouter_monthly_eur"));
  fill_value (m->el_cap, setting (m, "budgets.elevenlabs_chars_month"));
  JsonObject *keys = obj_of (m->st, "keys");
  keys_update (m);
  bat_update (m);
  ns_row_set_subtitle (m->key_pw, keys && json_object_get_boolean_member (keys, "opencode_password") ? "Present" : "Missing");
  JsonObject *b = obj_of (m->st, "budget");
  const char *cap = setting (m, "budgets.openrouter_monthly_eur");
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
  if (!m->picks_built)
    build_rows (m);
  chain_fill (m);
  notes_fill (m);
  update_fields (m);
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
  if (m->bat_timer)
    g_source_remove (m->bat_timer);
  if (m->st)
    json_object_unref (m->st);
  g_strfreev (m->chain_ids);
  g_free (m);
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
  ns_note (c, "Tried in this order when the main model cannot answer. OpenRouter models fall back inside OpenRouter; "
              "free opencode models are the last resort when OpenRouter is out of reach.");

  m->roles_g = ns_group (c, "Models by role");
  ns_note (c, "Eyes and Mailbox are saved, but nothing reads them yet.");

  m->voice_g = ns_group (c, "Voice");

  GtkWidget *bg = ns_group (c, "Budgets");
  m->or_cap = field_row (m, bg, "OpenRouter per month (EUR)", "budgets.openrouter_monthly_eur");
  m->el_cap = field_row (m, bg, "ElevenLabs per month (characters)", "budgets.elevenlabs_chars_month");
  m->spent = ns_row_info (bg, "Spent on OpenRouter this month", "");
  ns_note (c, "Empty or 0 means no paid use: OpenRouter stays off, and only the free models answer, until you set a "
              "cap. Over the cap, Minerva falls back to the free models until the month turns. Checked hourly.");

  GtkWidget *tg = ns_group (c, "Battery");
  m->bat_val = ns_row_info (tg, "Max charge", "");
  ns_row_scale (tg, NULL, "battery-level-80-symbolic", 50, 100, 5, &m->bat_scale);
  gtk_range_set_round_digits (GTK_RANGE (m->bat_scale), 0);
  g_signal_connect (m->bat_scale, "value-changed", G_CALLBACK (bat_moved), m);
  m->bat_state = ns_row_info (tg, "Charging", "");
  gtk_widget_set_visible (m->bat_state, FALSE);
  ns_note (c, "Holds the battery near this level while plugged in; the phone reads the level from the battery voltage. "
              "100 % switches it off.");

  GtkWidget *kg = ns_group (c, "Keys");
  key_row_build (m, kg, &m->keys[0], "openrouter", "OpenRouter", "openrouter.env");
  key_row_build (m, kg, &m->keys[1], "elevenlabs", "ElevenLabs", "elevenlabs.env");
  m->key_pw = ns_row_info (kg, "Agent server password", "");
  ns_note (c, "Tap a key to enter it; it is never shown again, only its last four characters. Without the OpenRouter key "
              "only the free opencode models are used. ElevenLabs is stored for the voice service.");

  say (m, "", FALSE);
  mn_refresh (m);
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
  static const char *const engines[] = { "Phone voice only (Piper)", "ElevenLabs, Piper as fallback", NULL };
  ns_row_pick (m->voice_g, "Speech engine", engines, !g_strcmp0 (setting (m, "voice.engine"), "elevenlabs"),
               engine_picked, m);
  m->voice_id = field_row (m, m->voice_g, "ElevenLabs voice", "voice.voice");
  m->voice_cap = field_row (m, m->voice_g, "Characters per day", "voice.daily_chars");
}
