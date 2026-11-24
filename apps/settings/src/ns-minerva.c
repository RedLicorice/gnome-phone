/* The Minerva page: which model the phone's agent thinks with, its fallbacks, per-role models, voice, budgets, and
 * whether the keys are there. Everything is read and written through `minerva-config` (Minerva's repository,
 * config/): `status --json` to read, `set key=value --apply --json` to write. The page keeps no state of its own,
 * and never shows a key: it takes one in a password dialog, hands it to `minerva-config key set` on stdin (never in
 * argv) and shows only "set, …last4" or "missing". */
#include <gio/gdesktopappinfo.h>
#include "ns-pages.h"
#include "ns-mc.h"
#include "ns-ui.h"

#include <json-glib/json-glib.h>

/* the Contacts app and its action that opens the people's Minerva settings (its desktop file's Actions) */
#define CONTACTS_DESKTOP "de.yesman.Contacts.desktop"
#define CONTACTS_TRUST_ACTION "minerva-settings"

#define str_of ns_json_str
#define obj_of ns_json_obj
#define arr_of ns_json_arr

typedef struct Mn Mn;

/* which of Minerva's pages this is: the index and the pages it lists. Each builds only the cards it has, so a
 * refresh fills what is there and skips the rest. MN_GROUPS is a page made only of data: the setting groups and the
 * link rows that name its id (`gpage`), Minerva's own (Memoria, Rete intelligente) or a system page's (Display,
 * Connessioni...) that takes them in (ns_minerva_embed). */
typedef enum { MN_INDEX, MN_BRAIN, MN_VOICE, MN_PROMPT, MN_BUDGET, MN_GROUPS } MnKind;

struct Mn {
  MnKind     kind;
  char      *gpage;                     /* MN_GROUPS: the page id its groups and links name */
  gboolean   embedded;                  /* MN_GROUPS inside a system page: the page's title is the system page's */
  AdwNavigationPage *page;
  GtkWidget *content;                   /* the page's column: its cards are appended here */
  GtkWidget *msg;                       /* the last result: saved, restarted, or the error */
  GtkWidget *brain_g, *roles_g, *voice_g; /* filled once, with the first status (the pickers need the model list) */
  GtkWidget *chain, *stt, *notes;       /* rebuilt on every refresh (no pickers inside) */
  GtkWidget *stt_timeout;               /* the cloud recognizers' time limit row */
  GtkWidget *voice_id, *voice_cap, *budget_g, *spent; /* rows updated in place */
  GtkWidget *events, *bypass;           /* the events daemon's day: tokens used, and today's bypass switch */
  GtkWidget *sg_box;                    /* the data-driven groups (setting_groups of the status) go in here */
  GtkWidget *status_model, *status_spend; /* the index's status card: the model in use, this month's spend */
  GtkWidget *vp_info;                   /* the voiceprint's state row (a group's "voiceprint" data) */
  char      *vp_text[3];                /* its texts: enrolled, none, enrolling ({got}, {need}) */
  GPtrArray *vp_actions;                /* the voiceprint buttons: "mn-when" says in which state each shows */
  GtkWidget *ph_info;                   /* the owner's secret phrase: its row (a group's "phrase" data) */
  char      *ph_help, *ph_remote;       /* its subtitle: the help, or the remote recogniser's warning ({engines}) */
  GtkWidget *nm_g;                      /* her names: the main one and the nicknames, rebuilt on every refresh */
  JsonObject *nm;                       /* their texts and the save command (a group's "names" data) */
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
  gboolean   entries_built;             /* the index's rows are built once, from minerva_ui.json */
  gboolean   busy;
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

/* the index page's own texts (minerva_ui.json, through the status) */
static JsonObject *
mn_ui (Mn *m)
{
  return obj_of (m->st, "minerva_ui");
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
static void build_page (Mn *m);
static void index_fill (Mn *m);

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
  if (m->st && m->stt)
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

/* ---- the owner's secret phrase (Minerva's docs/secret-phrase.md): a row and two buttons, every text from the data.
 * The phrase is typed once in a password dialog and goes to `minerva-config people phrase set` on stdin; the page
 * never keeps or shows it, and never says whether one is set. minerva-config asks the owner's finger or PIN on the
 * gate sheet before the memory service takes it. ---- */

typedef struct {
  Mn       *m;
  char     *heading, *body, *ok, *done;
  gboolean  entry, destructive;   /* set: a password entry first; remove: a confirmation */
  GStrv     args;
} PhAction;

static void
ph_action_free (PhAction *a)
{
  g_free (a->heading);
  g_free (a->body);
  g_free (a->ok);
  g_free (a->done);
  g_strfreev (a->args);
  g_free (a);
}

static void
ph_done (Mn *m, JsonObject *root, const char *err, gpointer data)
{
  PhAction *a = data;
  if (err)
    say (m, err, TRUE);
  else if (root && json_object_has_member (root, "ok") && !json_object_get_boolean_member (root, "ok"))
    say (m, str_of (root, "error"), TRUE);
  else
    say (m, a->done, FALSE);
}

static void
ph_entered (const char *text, gpointer data)
{
  PhAction *a = data;
  if (text && *text)
    mn_run_in (a->m, (const char *const *) a->args, text, ph_done, a);
}

static void
ph_answered (gboolean yes, gpointer data)
{
  PhAction *a = data;
  if (yes)
    mn_run (a->m, (const char *const *) a->args, ph_done, a);
}

static void
ph_action_run (GtkWidget *row, gpointer data)
{
  PhAction *a = data;
  if (a->entry)
    ns_ask (row, a->heading, a->body, NULL, TRUE, a->ok, ph_entered, a);
  else
    ns_confirm (row, a->heading, a->body, a->ok, a->destructive, ph_answered, a);
}

static void
ph_button (Mn *m, GtkWidget *grp, JsonObject *o, gboolean entry)
{
  if (!o)
    return;
  PhAction *a = g_new0 (PhAction, 1);
  a->m = m;
  a->heading = g_strdup (str_of (o, "heading"));
  a->body = g_strdup (str_of (o, "body"));
  a->ok = g_strdup (str_of (o, "ok"));
  a->done = g_strdup (str_of (o, "done"));
  a->entry = entry;
  a->destructive = json_object_get_boolean_member_with_default (o, "destructive", FALSE);
  JsonArray *av = arr_of (o, "args");
  g_autoptr (GPtrArray) v = g_ptr_array_new ();
  for (guint k = 0; av && k < json_array_get_length (av); k++)
    g_ptr_array_add (v, g_strdup (json_array_get_string_element (av, k)));
  g_ptr_array_add (v, NULL);
  a->args = g_strdupv ((char **) v->pdata);
  GtkWidget *row = ns_row_button (grp, str_of (o, "label"), a->destructive, ph_action_run, a);
  g_object_set_data_full (G_OBJECT (row), "mn-ph-action", a, (GDestroyNotify) ph_action_free);
}

static void
build_phrase (Mn *m, GtkWidget *grp, JsonObject *ph)
{
  g_free (m->ph_help);
  g_free (m->ph_remote);
  m->ph_help = g_strdup (str_of (ph, "help"));
  m->ph_remote = g_strdup (str_of (ph, "remote"));
  m->ph_info = ns_row_info (grp, str_of (ph, "label"), m->ph_help);
  ph_button (m, grp, obj_of (ph, "set"), TRUE);
  ph_button (m, grp, obj_of (ph, "remove"), FALSE);
}

/* the status's "phrase": {local, remote: [engines]}; a remote recogniser in the chain gives the phrase no weight */
static void
fill_phrase (Mn *m)
{
  JsonObject *ph = obj_of (m->st, "phrase");
  if (!ph || !m->ph_info || !json_object_has_member (ph, "local"))
    return;
  if (json_object_get_boolean_member_with_default (ph, "local", FALSE)) {
    ns_row_set_subtitle (m->ph_info, m->ph_help);
    return;
  }
  JsonArray *remote = arr_of (ph, "remote");
  g_autoptr (GString) names = g_string_new (NULL);
  for (guint i = 0; remote && i < json_array_get_length (remote); i++)
    g_string_append_printf (names, "%s%s", i ? ", " : "", json_array_get_string_element (remote, i));
  g_autoptr (GString) t = g_string_new (m->ph_remote);
  g_string_replace (t, "{engines}", names->str, 0);
  ns_row_set_subtitle (m->ph_info, t->str);
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
launch_app_row (GtkWidget *row, gpointer data)
{
  g_autoptr (GDesktopAppInfo) info = g_desktop_app_info_new (g_object_get_data (G_OBJECT (row), "mn-desktop"));
  g_autoptr (GError) error = NULL;

  (void) data;
  if (info && !g_app_info_launch (G_APP_INFO (info), NULL, NULL, &error))
    g_message ("%s: %s", g_app_info_get_id (G_APP_INFO (info)), error ? error->message : "not launched");
}

/* a group's "app": {"desktop": id, "help": subtitle}: the app's own row (its name and icon), only when installed */
static void
build_app_row (GtkWidget *grp, JsonObject *ap)
{
  g_autoptr (GDesktopAppInfo) info = g_desktop_app_info_new (str_of (ap, "desktop"));
  GtkWidget *row = NULL;

  if (!info)
    return;
  row = ns_row_app (grp, G_APP_INFO (info), str_of (ap, "help"));
  g_object_set_data_full (G_OBJECT (row), "mn-desktop", g_strdup (str_of (ap, "desktop")), g_free);
  ns_row_on_activate (row, launch_app_row, NULL);
}

/* ---- her names (Parola di attivazione): the main name, read only (the assistant's identity), and the owner's
 * nicknames, each with the phrases it lets him say. The list and the phrases come with the status ("names":
 * `minerva-config names`), the texts and the save command with the group ("names" of settings_ui.json). A change saves
 * the whole list of nicknames: the data's args, then every nickname in order; minerva-config checks it (empty, twice,
 * a command word, another assistant's name) and its refusal is shown as it says it. ---- */

typedef struct {
  Mn    *m;
  guint  index;    /* the nickname's place in the list; the list's length: a new one */
  char  *entry;    /* the nickname as it is now ("" for a new one) */
} NmOp;

static void
nm_op_free (NmOp *op)
{
  g_free (op->entry);
  g_free (op);
}

static NmOp *
nm_op_new (Mn *m, guint index, const char *entry)
{
  NmOp *op = g_new0 (NmOp, 1);
  op->m = m;
  op->index = index;
  op->entry = g_strdup (entry);
  return op;
}

/* the data's text `key` of `o` with `{what}` replaced by `value` */
static char *
nm_text (JsonObject *o, const char *key, const char *what, const char *value)
{
  g_autoptr (GString) t = g_string_new (str_of (o, key));
  g_string_replace (t, what, value, 0);
  return g_string_free (g_steal_pointer (&t), FALSE);
}

/* "Sveglia: «ehi mimi» • In conversazione: «mimi»": the phrases of an entry, each list in its own words */
static void
nm_forms_append (GString *s, JsonObject *ui, JsonArray *forms, const char *label)
{
  if (!forms || !json_array_get_length (forms))
    return;
  g_autoptr (GString) list = g_string_new (NULL);
  for (guint i = 0; i < json_array_get_length (forms); i++) {
    g_autofree char *f = nm_text (ui, "form", "{form}", json_array_get_string_element (forms, i));
    g_string_append_printf (list, "%s%s", i ? str_of (ui, "forms_sep") : "", f);
  }
  g_autofree char *part = nm_text (ui, label, "{forms}", list->str);
  g_string_append_printf (s, "%s%s", s->len ? str_of (ui, "parts_sep") : "", part);
}

static char *
nm_forms (JsonObject *ui, JsonObject *entry)
{
  GString *s = g_string_new (NULL);
  nm_forms_append (s, ui, arr_of (entry, "wake"), "wake");
  nm_forms_append (s, ui, arr_of (entry, "attention"), "attention");
  return g_string_free (s, FALSE);
}

static void
nm_done (Mn *m, JsonObject *root, const char *err, gpointer data)
{
  m->busy = FALSE;
  say (m, err ? err : str_of (m->nm, "saved"), err != NULL);
  mn_refresh (m);
}

/* save the nicknames of the status with the one at `index` set to `value` (NULL: dropped; index past the end: added) */
static void
nm_save (Mn *m, guint index, const char *value)
{
  if (m->busy) {
    say (m, "Still applying the last change…", FALSE);
    return;
  }
  JsonArray *nicks = arr_of (obj_of (m->st, "names"), "nicknames"), *args = arr_of (m->nm, "args");
  guint n = nicks ? json_array_get_length (nicks) : 0;
  g_autoptr (GPtrArray) v = g_ptr_array_new ();
  for (guint k = 0; args && k < json_array_get_length (args); k++)
    g_ptr_array_add (v, (gpointer) json_array_get_string_element (args, k));
  for (guint i = 0; i < n; i++) {
    const char *e = str_of (json_array_get_object_element (nicks, i), "entry");
    if (i != index)
      g_ptr_array_add (v, (gpointer) e);
    else if (value)
      g_ptr_array_add (v, (gpointer) value);
  }
  if (index >= n && value)
    g_ptr_array_add (v, (gpointer) value);
  g_ptr_array_add (v, NULL);
  m->busy = TRUE;
  say (m, str_of (m->nm, "saving"), FALSE);
  mn_run (m, (const char *const *) v->pdata, nm_done, NULL);
}

static void
nm_entered (const char *text, gpointer data)
{
  NmOp *op = data;
  if (text && g_strcmp0 (text, op->entry))
    nm_save (op->m, op->index, text);   /* an empty one goes too: minerva-config says why it is refused */
  nm_op_free (op);
}

static void
nm_answered (gboolean yes, gpointer data)
{
  NmOp *op = data;
  if (yes)
    nm_save (op->m, op->index, NULL);
  nm_op_free (op);
}

/* the dialogs get their own copy of the row's op: a refresh may rebuild the rows while one is open */
static void
nm_rename (GtkButton *b, gpointer data)
{
  NmOp *op = data;
  JsonObject *t = obj_of (op->m->nm, "rename");
  ns_ask (GTK_WIDGET (b), str_of (t, "heading"), str_of (t, "body"), op->entry, FALSE, str_of (t, "ok"), nm_entered,
          nm_op_new (op->m, op->index, op->entry));
}

static void
nm_remove (GtkButton *b, gpointer data)
{
  NmOp *op = data;
  JsonObject *t = obj_of (op->m->nm, "remove");
  g_autofree char *heading = nm_text (t, "heading", "{name}", op->entry);
  ns_confirm (GTK_WIDGET (b), heading, str_of (t, "body"), str_of (t, "ok"), TRUE, nm_answered,
              nm_op_new (op->m, op->index, op->entry));
}

static void
nm_add (GtkWidget *row, gpointer data)
{
  NmOp *op = data;
  JsonObject *t = obj_of (op->m->nm, "add");
  ns_ask (row, str_of (t, "heading"), str_of (t, "body"), NULL, FALSE, str_of (t, "ok"), nm_entered,
          nm_op_new (op->m, op->index, ""));
}

static void
fill_names (Mn *m)
{
  JsonObject *st = obj_of (m->st, "names");
  if (!m->nm_g || !st)
    return;
  ns_group_clear (m->nm_g);
  JsonArray *own = arr_of (st, "own"), *nicks = arr_of (st, "nicknames");
  /* the main name: one row, its own entries' phrases together */
  g_autoptr (GString) sub = g_string_new (str_of (m->nm, "main"));
  for (guint i = 0; own && i < json_array_get_length (own); i++) {
    g_autofree char *f = nm_forms (m->nm, json_array_get_object_element (own, i));
    g_string_append_printf (sub, "%s%s", str_of (m->nm, "parts_sep"), f);
  }
  ns_row (m->nm_g, NULL, NULL, str_of (st, "name"), sub->str);
  guint n = nicks ? json_array_get_length (nicks) : 0;
  for (guint i = 0; i < n; i++) {
    JsonObject *e = json_array_get_object_element (nicks, i);
    g_autofree char *f = nm_forms (m->nm, e);
    GtkWidget *row = ns_row (m->nm_g, NULL, NULL, str_of (e, "entry"), f);
    NmOp *op = nm_op_new (m, i, str_of (e, "entry"));
    g_object_set_data_full (G_OBJECT (row), "mn-nm-op", op, (GDestroyNotify) nm_op_free);
    GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_append (GTK_BOX (box), icon_button ("document-edit-symbolic", str_of (obj_of (m->nm, "rename"), "tip"),
                                                G_CALLBACK (nm_rename), op, 0, TRUE));
    gtk_box_append (GTK_BOX (box), icon_button ("list-remove-symbolic", str_of (obj_of (m->nm, "remove"), "tip"),
                                                G_CALLBACK (nm_remove), op, 0, TRUE));
    ns_row_suffix (row, box);
  }
  gint64 max = json_object_get_int_member_with_default (st, "max_nicknames", 0);
  if ((gint64) n < max) {
    NmOp *op = nm_op_new (m, n, "");
    GtkWidget *add = ns_row_button (m->nm_g, str_of (obj_of (m->nm, "add"), "label"), FALSE, nm_add, op);
    g_object_set_data_full (G_OBJECT (add), "mn-nm-op", op, (GDestroyNotify) nm_op_free);
  } else {
    g_autofree char *count = g_strdup_printf ("%" G_GINT64_FORMAT, max);
    g_autofree char *full = nm_text (m->nm, "full", "{count}", count);
    ns_row (m->nm_g, NULL, NULL, full, NULL);
  }
}

/* the groups of one page: a group of settings_ui.json names its page, the Voice page when it names none; `after`
 * takes the ones placed after the page's own cards ("place": "after_engines": the Voice page's Comandi, Avanzate) */
static void
build_groups (Mn *m, const char *page, gboolean after)
{
  JsonArray *gs = arr_of (m->st, "setting_groups");
  for (guint i = 0; gs && i < json_array_get_length (gs); i++) {
    JsonObject *g = json_array_get_object_element (gs, i);
    const char *gp = str_of (g, "page");
    if (g_strcmp0 (*gp ? gp : "voice", page) || (*str_of (g, "place") != '\0') != after)
      continue;
    JsonObject *nm = obj_of (g, "names");   /* her names: a card of their own, first under the group's title */
    if (nm && !m->nm) {
      m->nm = json_object_ref (nm);
      m->nm_g = ns_group (m->sg_box, str_of (g, "title"));
    }
    GtkWidget *grp = ns_group (m->sg_box, nm ? NULL : str_of (g, "title"));
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
    JsonObject *ph = obj_of (g, "phrase");
    if (ph)
      build_phrase (m, grp, ph);
    JsonObject *ap = obj_of (g, "app");   /* a row that opens an app of the group's subject (Neo Voice) */
    if (ap)
      build_app_row (grp, ap);
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

/* the budget in money (the first budget row with a price): the index page has no budget rows of its own, so it
 * looks it up in the status instead */
static const char *
money_cap_key (Mn *m)
{
  JsonArray *br = arr_of (m->st, "budget_rows");
  for (guint i = 0; br && i < json_array_get_length (br); i++) {
    JsonObject *o = json_array_get_object_element (br, i);
    if (json_object_get_boolean_member (o, "money"))
      return str_of (o, "key");
  }
  return NULL;
}

/* this month's spend, as Budget's row and the index's status card show it */
static char *
spend_text (Mn *m)
{
  JsonObject *b = obj_of (m->st, "budget"), *labels = obj_of (mn_ui (m), "status");
  const char *cap_key = m->cap_key ? m->cap_key : money_cap_key (m);
  const char *cap = cap_key ? setting (m, cap_key) : "";
  if (b && json_object_has_member (b, "usage_usd") && !json_node_is_null (json_object_get_member (b, "usage_usd"))) {
    gboolean over = json_object_get_boolean_member (b, "over");
    return g_strdup_printf ("$%.2f%s%s%s%s", json_object_get_double_member (b, "usage_usd"),
                            *cap ? " of €" : "", cap, over ? " • " : "", over ? str_of (labels, "over_cap") : "");
  }
  return g_strdup (*cap && g_ascii_strtod (cap, NULL) > 0 ? str_of (labels, "not_checked")
                                                          : str_of (labels, "no_cap"));
}

/* the model in use, named as the pickers name it: the effective one, else the configured one */
static char *
model_text (Mn *m)
{
  const char *id = str_of (obj_of (m->st, "effective"), "main");
  if (!*id)
    id = setting (m, "brain.main");
  return *id ? id_label (m, id) : g_strdup (str_of (obj_of (mn_ui (m), "status"), "no_model"));
}

static void
update_fields (Mn *m)
{
  if (m->voice_id)
    fill_value (m->voice_id, setting (m, "voice.voice"));
  if (m->voice_cap)
    fill_value (m->voice_cap, setting (m, "voice.daily_chars"));
  if (m->stt_timeout)
    fill_value (m->stt_timeout, setting (m, "voice.stt_timeout_s"));
  for (guint i = 0; m->caps && i < m->caps->len; i++)
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
  if (m->spent) {
    g_autofree char *t = spend_text (m);
    ns_row_set_subtitle (m->spent, t);
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
  if (m->kind == MN_INDEX) {
    index_fill (m);
    return;
  }
  if (!m->picks_built) {
    m->picks_built = TRUE;
    build_page (m);
  }
  if (m->chain)
    chain_fill (m);
  if (m->stt)
    stt_fill (m);
  if (m->notes)
    notes_fill (m);
  update_fields (m);
  fill_groups (m);
  fill_voiceprint (m);
  fill_phrase (m);
  fill_names (m);
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
  g_free (m->ph_help);
  g_free (m->ph_remote);
  g_free (m->gpage);
  if (m->nm)
    json_object_unref (m->nm);
  g_free (m);
}

/* ---- the pages: the index and the four it lists, all built from minerva_ui.json ----
 * The index shows the model in use and this month's spend, then one row per page in the file's order, with its
 * icon, colour, title and subtitle. Each sub-page builds only the cards it has; a refresh fills what is there and
 * skips the rest. No setting, label or order lives in this file: they are lines of the data. */

/* which page a key of minerva_ui.json's "pages" opens: the data names it, this table says where it goes. One entry
 * opens a document in Neo Prompt instead of a sub-page (Minerva's own notebook, which is hers to edit). */
static void
open_self_notebook (GtkWidget *row, gpointer data)
{
  (void) row;
  (void) data;
  ns_prompt_open ("self-notebook", NULL);
}

/* the people's rules (trust, when strangers are forgotten) are the Contacts app's own page: its action opens it */
static void
open_contacts_trust (GtkWidget *row, gpointer data)
{
  (void) row;
  (void) data;
  g_autoptr (GDesktopAppInfo) info = g_desktop_app_info_new (CONTACTS_DESKTOP);
  if (info)
    g_desktop_app_info_launch_action (info, CONTACTS_TRUST_ACTION, NULL);
}

/* an opener of another app's page: shown only while that app is there (a row that does nothing is worse than none) */
static gboolean
opener_available (const char *id)
{
  if (!g_strcmp0 (id, "self_notebook"))
    return ns_prompt_installed ();
  if (!g_strcmp0 (id, "contacts_trust")) {
    g_autoptr (GDesktopAppInfo) info = g_desktop_app_info_new (CONTACTS_DESKTOP);
    const char *const *actions = info ? g_desktop_app_info_list_actions (info) : NULL;
    return actions && g_strv_contains (actions, CONTACTS_TRUST_ACTION);
  }
  return TRUE;
}

static const struct {
  const char *id;
  NsActivate  page;
} minerva_pages[] = {
  { "brain", ns_page_minerva_brain },
  { "voice", ns_page_minerva_voice },
  { "prompt", ns_page_prompt },
  { "events", ns_page_events },
  { "messaging", ns_page_messaging },
  { "federation", ns_page_federation },
  { "budget", ns_page_minerva_budget },
  { "memory", ns_page_minerva_memory },
  { "smart_network", ns_page_minerva_smart_network },
  { "avatar", ns_page_minerva_avatar },
  { "self_notebook", open_self_notebook },
  { "contacts_trust", open_contacts_trust },
};

static NsActivate
page_opener (const char *id)
{
  for (guint k = 0; k < G_N_ELEMENTS (minerva_pages); k++)
    if (!g_strcmp0 (minerva_pages[k].id, id))
      return opener_available (id) ? minerva_pages[k].page : NULL;
  return NULL;
}

static JsonObject *
mn_ui_page (Mn *m, const char *id)
{
  JsonObject *p = obj_of (obj_of (mn_ui (m), "pages"), id);
  return p ? p : obj_of (obj_of (mn_ui (m), "subpages"), id);   /* a page of its own that is no index entry */
}

/* a sub-page's title, from the data (it arrives with the status: the page opens with a blank one) */
static void
page_title (Mn *m, const char *id)
{
  const char *t = str_of (mn_ui_page (m, id), "title");
  if (*t)
    ns_page_set_title (m->page, t);
}

/* a card's title: a missing or empty one in the data is an untitled card */
static const char *
ui_title (JsonObject *o, const char *name)
{
  const char *t = str_of (o, name);
  return *t ? t : NULL;
}

static void
note_if (JsonObject *notes, const char *name, GtkWidget *content)
{
  const char *t = str_of (notes, name);
  if (*t)
    ns_note (content, t);
}

/* the index's status card and its rows, once, with the first status */
static void
index_build (Mn *m)
{
  JsonObject *ui = mn_ui (m), *labels = obj_of (ui, "status"), *pages = obj_of (ui, "pages");
  GtkWidget *g = ns_group (m->content, NULL);
  m->status_model = ns_row_info (g, str_of (labels, "model"), "");
  m->status_spend = ns_row_info (g, str_of (labels, "spend"), "");
  JsonArray *order = arr_of (ui, "order");
  GtkWidget *eg = NULL;
  for (guint i = 0; order && i < json_array_get_length (order); i++) {
    const char *id = json_array_get_string_element (order, i);
    JsonObject *p = obj_of (pages, id);
    NsActivate page = page_opener (id);
    if (!page || !*str_of (p, "title"))
      continue;   /* a page this build does not have */
    if (!eg)
      eg = ns_group (m->content, NULL);
    GtkWidget *r = ns_row (eg, str_of (p, "icon"), str_of (p, "color"), str_of (p, "title"), str_of (p, "subtitle"));
    ns_row_on_activate (r, page, NULL);
  }
  m->entries_built = TRUE;
}

static void
index_fill (Mn *m)
{
  JsonObject *ui = mn_ui (m);
  if (!*str_of (ui, "title"))
    return;   /* no data file, or an older minerva-config: the page stays empty, nothing is guessed */
  ns_page_set_title (m->page, str_of (ui, "title"));
  if (!m->entries_built)
    index_build (m);
  g_autofree char *model = model_text (m), *spend = spend_text (m);
  ns_row_set_subtitle (m->status_model, model);
  ns_row_set_subtitle (m->status_spend, spend);
}

/* the sub-pages: their cards' titles and notes come from minerva_ui.json too */
static void
brain_build (Mn *m)
{
  JsonObject *p = mn_ui_page (m, "brain"), *s = obj_of (p, "sections"), *n = obj_of (p, "notes");
  page_title (m, "brain");
  m->brain_g = ns_group (m->content, ui_title (s, "model"));
  m->notes = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
  gtk_box_append (GTK_BOX (m->content), m->notes);
  m->chain = ns_group (m->content, ui_title (s, "chain"));
  note_if (n, "chain", m->content);
  m->roles_g = ns_group (m->content, ui_title (s, "roles"));
  note_if (n, "roles", m->content);
}

static void
voice_build (Mn *m)
{
  JsonObject *p = mn_ui_page (m, "voice"), *s = obj_of (p, "sections"), *n = obj_of (p, "notes");
  page_title (m, "voice");
  m->sg_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);   /* the wake word (the voiceprint), the reply */
  gtk_box_append (GTK_BOX (m->content), m->sg_box);
  build_groups (m, "voice", FALSE);
  m->voice_g = ns_group (m->content, ui_title (s, "engine"));   /* the engines */
  m->stt = ns_group (m->content, ui_title (s, "stt"));
  GtkWidget *sg = ns_group (m->content, NULL);
  m->stt_timeout = field_row (m, sg, "Cloud time limit (seconds)", "voice.stt_timeout_s");
  note_if (n, "stt", m->content);
  m->sg_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);   /* the commands, the advanced groups */
  gtk_box_append (GTK_BOX (m->content), m->sg_box);
  build_groups (m, "voice", TRUE);
  const char *const stt_args[] = { "stt-status", "--json", NULL };
  mn_run (m, stt_args, stt_status_done, NULL);   /* the Claude CLI's login, a second or so: shown when it comes */
}

/* the link rows minerva_ui.json's "links" gives page `id`: each opens a page or another app's (page_opener), its
 * title, subtitle, icon and colour its own or that page's */
static void
build_links (Mn *m, const char *id)
{
  JsonArray *links = arr_of (obj_of (mn_ui (m), "links"), id);
  GtkWidget *g = NULL;
  for (guint i = 0; links && i < json_array_get_length (links); i++) {
    JsonObject *l = json_array_get_object_element (links, i), *target = mn_ui_page (m, str_of (l, "open"));
    NsActivate open = page_opener (str_of (l, "open"));
    if (!open)
      continue;
#define LINK_FIELD(f) (*str_of (l, f) ? str_of (l, f) : str_of (target, f))
    if (!g)
      g = ns_group (m->sg_box, NULL);
    GtkWidget *r = ns_row (g, LINK_FIELD ("icon"), LINK_FIELD ("color"), LINK_FIELD ("title"), LINK_FIELD ("subtitle"));
#undef LINK_FIELD
    ns_row_on_activate (r, open, NULL);
  }
}

/* a page of data only: its groups, then its links (Memoria; Rete intelligente; a system page's part) */
static void
groups_build (Mn *m)
{
  if (!m->embedded) {
    page_title (m, m->gpage);
    note_if (mn_ui_page (m, m->gpage), "note", m->content);
  }
  m->sg_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_append (GTK_BOX (m->content), m->sg_box);
  build_groups (m, m->gpage, FALSE);
  build_groups (m, m->gpage, TRUE);
  build_links (m, m->gpage);
}

static void
budget_build (Mn *m)
{
  JsonObject *p = mn_ui_page (m, "budget"), *s = obj_of (p, "sections"), *n = obj_of (p, "notes");
  page_title (m, "budget");
  m->cap_key = g_strdup (money_cap_key (m));   /* the spent line is compared with it, the cap row or not */
  m->budget_g = ns_group (m->content, ui_title (s, "budget"));
  note_if (n, "budget", m->content);
}

static void
prompt_build (Mn *m)
{
  page_title (m, "prompt");
  ns_prompt_build (m->content, G_OBJECT (m->page), obj_of (m->st, "prompt_ui"));
}

/* called once, with the first status: the page's cards, then the pickers that need the model list */
static void
build_page (Mn *m)
{
  switch (m->kind) {
  case MN_BRAIN: brain_build (m); break;
  case MN_VOICE: voice_build (m); break;
  case MN_BUDGET: budget_build (m); break;
  case MN_GROUPS: groups_build (m); break;
  case MN_PROMPT: prompt_build (m); break;
  default: break;
  }
  build_rows (m);
}

/* a page of Minerva's: its title and its texts arrive with the status, so it opens blank and fills */
static Mn *
mn_new (MnKind kind, GtkWidget **content)
{
  AdwNavigationPage *page = ns_page_new ("", content);
  Mn *m = g_new0 (Mn, 1);
  m->kind = kind;
  m->page = page;
  m->content = *content;
  g_object_set_data_full (G_OBJECT (page), "mn", m, (GDestroyNotify) mn_free);
  m->msg = ns_note (*content, "");
  gtk_widget_set_visible (m->msg, FALSE);
  gtk_widget_set_margin_top (m->msg, 0);
  m->srows = g_ptr_array_new ();
  m->vp_actions = g_ptr_array_new ();
  return m;
}

static void
mn_page_open (MnKind kind)
{
  GtkWidget *c;
  Mn *m = mn_new (kind, &c);
  mn_refresh (m);
  ns_push (m->page);
}

static void
mn_groups_open (const char *id)
{
  GtkWidget *c;
  Mn *m = mn_new (MN_GROUPS, &c);
  m->gpage = g_strdup (id);
  mn_refresh (m);
  ns_push (m->page);
}

/* Minerva's part of a system page (Connessioni, Display, Blocco schermo, Notifiche, Rete mobile): the setting groups
 * and link rows of minerva_ui.json and settings_ui.json that name `id`, appended to `content` when the status comes.
 * The same rows as Minerva's own pages, reading and writing the same keys; nothing when Minerva is not there. */
void
ns_minerva_embed (AdwNavigationPage *page, GtkWidget *content, const char *id)
{
  if (!ns_minerva_available ())
    return;
  Mn *m = g_new0 (Mn, 1);
  m->kind = MN_GROUPS;
  m->gpage = g_strdup (id);
  m->embedded = TRUE;
  m->page = page;
  m->content = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_append (GTK_BOX (content), m->content);
  g_object_set_data_full (G_OBJECT (page), "mn-embed", m, (GDestroyNotify) mn_free);
  m->msg = ns_note (m->content, "");
  gtk_widget_set_visible (m->msg, FALSE);
  m->srows = g_ptr_array_new ();
  m->vp_actions = g_ptr_array_new ();
  mn_refresh (m);
}

/* Minerva (`--page minerva`): the index. Model in use and this month's spend, then the pages */
void
ns_page_minerva (GtkWidget *row, gpointer data)
{
  mn_page_open (MN_INDEX);
}

/* Minerva > Cervello (`--page brain`): the model, its fallback chain, the models by role */
void
ns_page_minerva_brain (GtkWidget *row, gpointer data)
{
  mn_page_open (MN_BRAIN);
}

/* Minerva > Voce (`--page voice`): the speech engines, the recognizers, the wake word and the reply */
void
ns_page_minerva_voice (GtkWidget *row, gpointer data)
{
  mn_page_open (MN_VOICE);
}

/* Minerva > Budget (`--page budget`): every quota, the events' day and their bypass */
void
ns_page_minerva_budget (GtkWidget *row, gpointer data)
{
  mn_page_open (MN_BUDGET);
}

/* Minerva > Memoria (`--page memory`): what she learns, her recall, her notebook (Neo Prompt), the people's trust
 * (Contacts) */
void
ns_page_minerva_memory (GtkWidget *row, gpointer data)
{
  mn_groups_open ("memory");
}

/* Connessioni > Rete intelligente (`--page smart-network`): Wi-Fi first, mobile data, the internet check, the repair
 * ladder (minerva-net) */
void
ns_page_minerva_smart_network (GtkWidget *row, gpointer data)
{
  mn_groups_open ("smart_network");
}

/* Minerva > Prompt (`--page prompt`): the system prompt and the state template (ns-prompt.c) */
void
ns_page_prompt (GtkWidget *row, gpointer data)
{
  mn_page_open (MN_PROMPT);
}

/* the pickers and the voice card need the model list and the values, so they wait for the first status. A page has
 * only its own cards (the others are NULL), so each block checks for its group: the index and the Prompt page build
 * none. */
static void
build_rows (Mn *m)
{
  m->picks_built = TRUE;
  if (m->brain_g)
    choice_row (m, m->brain_g, "Main model", "brain.main", NULL, FALSE);
  if (m->roles_g) {
    choice_row (m, m->roles_g, "Messages", "roles.chat", "Same as main", FALSE);
      choice_row (m, m->roles_g, "Eyes (looks at pictures)", "roles.eyes", "Same as main", TRUE);
    choice_row (m, m->roles_g, "Mailbox (other agents)", "roles.mailbox", "Same as main", FALSE);
  }
  if (m->voice_g) {
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
  }
  if (m->budget_g) {
    m->caps = g_ptr_array_new ();
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
