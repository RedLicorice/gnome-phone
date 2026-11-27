/* Settings > Backup: where this node's backup goes. The destinations are the accounts linked in Accounts whose
 * provider Minerva's backup knows (phones never keep backups); the owner picks one or more, and every run goes to all
 * the picked ones. Everything comes from `minerva-config backup status --json` (Minerva's
 * config/minerva_config/backup.py: texts, destinations, the last run); a pick is `backup choose ID...`, "run now"
 * is `backup now`. The page keeps no state of its own. */
#include "ns-pages.h"
#include "ns-mc.h"
#include "ns-ui.h"

typedef struct {
  AdwNavigationPage *page;
  GtkWidget  *content, *state, *dests, *note, *actions, *msg;
  GPtrArray  *checks;      /* the destinations' check buttons, each with its id ("ns-dest") */
  JsonObject *st;          /* the last status */
  guint       poll_id;     /* reads the status again while a run started here goes on */
  gint64      started_at;  /* the last run's time when "run now" was pressed: a newer one ends the polling */
  gboolean    syncing, built;
} Bk;

static void bk_refresh (Bk *b);

static void
bk_free (Bk *b)
{
  if (b->poll_id)
    g_source_remove (b->poll_id);
  g_clear_pointer (&b->checks, g_ptr_array_unref);
  g_clear_pointer (&b->st, json_object_unref);
  g_free (b);
}

static const char *
text (Bk *b, const char *key)
{
  return ns_json_str (ns_json_obj (b->st, "texts"), key);
}

static const char *
status_text (Bk *b, const char *key)
{
  return ns_json_str (ns_json_obj (ns_json_obj (b->st, "texts"), "status"), key);
}

/* a reason in the owner's language when the texts know its id, else as the backup said it */
static const char *
reason (Bk *b, const char *id, const char *said)
{
  const char *t = ns_json_str (ns_json_obj (ns_json_obj (b->st, "texts"), "reasons"), id ? id : "");
  return *t ? t : said;
}

/* a text with {when} (a Unix time in the texts' time_format), {mb}, {n} and {why} filled in */
static char *
fill (Bk *b, const char *t, gint64 when, gint64 mb, const char *why)
{
  g_autoptr (GString) s = g_string_new (t ? t : "");
  if (when > 0) {
    g_autoptr (GDateTime) dt = g_date_time_new_from_unix_local (when);
    g_autofree char *w = g_date_time_format (dt, text (b, "time_format"));
    g_string_replace (s, "{when}", w ? w : "", 0);
  }
  g_autofree char *m = g_strdup_printf ("%" G_GINT64_FORMAT, mb);
  g_string_replace (s, "{mb}", m, 0);
  g_string_replace (s, "{n}", m, 0);
  g_string_replace (s, "{why}", why ? why : "", 0);
  return g_string_free (g_steal_pointer (&s), FALSE);
}

/* the last run's result for one destination, or NULL */
static JsonObject *
last_of (Bk *b, const char *id)
{
  JsonArray *a = ns_json_arr (ns_json_obj (b->st, "last"), "destinations");
  for (guint i = 0; a && i < json_array_get_length (a); i++) {
    JsonObject *r = json_array_get_object_element (a, i);
    if (!g_strcmp0 (ns_json_str (r, "id"), id))
      return r;
  }
  return NULL;
}

static void
chosen (JsonObject *root, const char *err, gpointer data)
{
  Bk *b = data;
  gtk_label_set_text (GTK_LABEL (b->msg), err ? err : "");
  gtk_widget_set_visible (b->msg, err != NULL);
  bk_refresh (b);
}

/* a check changed: every destination checked now is the pick */
static void
toggled (GtkCheckButton *c, Bk *b)
{
  if (b->syncing)
    return;
  g_autoptr (GStrvBuilder) args = g_strv_builder_new ();
  g_strv_builder_add_many (args, "backup", "choose", NULL);
  for (guint i = 0; i < b->checks->len; i++)
    if (gtk_check_button_get_active (b->checks->pdata[i]))
      g_strv_builder_add (args, g_object_get_data (G_OBJECT (b->checks->pdata[i]), "ns-dest"));
  g_strv_builder_add (args, "--json");
  g_auto (GStrv) v = g_strv_builder_end (args);
  ns_mc_run (G_OBJECT (b->page), (const char *const *) v, NULL, chosen, b);
}

static void
show (Bk *b)
{
  JsonObject *last = ns_json_obj (b->st, "last");
  JsonArray *ds = ns_json_arr (b->st, "destinations");
  int picked = 0;

  ns_page_set_title (b->page, text (b, "title"));
  for (guint i = 0; ds && i < json_array_get_length (ds); i++)
    picked += json_object_get_boolean_member_with_default (json_array_get_object_element (ds, i), "picked", FALSE);

  /* the state: the last run, then how many are picked */
  gint64 at = last ? json_object_get_int_member_with_default (last, "at", 0) : 0;
  const char *key = !last ? "never" : *ns_json_str (last, "skipped") ? "skipped"
                    : json_object_get_boolean_member_with_default (last, "ok", FALSE) ? "ok" : "failed";
  g_autofree char *title = fill (b, status_text (b, key), at, 0, NULL);
  g_autofree char *sub = picked ? fill (b, status_text (b, "picked"), 0, picked, NULL)
                                : g_strdup (status_text (b, "none_picked"));
  if (!g_strcmp0 (key, "skipped") && picked) {
    g_free (sub);
    sub = g_strdup (reason (b, ns_json_str (last, "skipped_id"), ns_json_str (last, "skipped")));
  }
  ns_row_set_subtitle (b->state, sub);
  gtk_label_set_text (GTK_LABEL (g_object_get_data (G_OBJECT (b->state), "ns-title-label")), title);

  /* the destinations: one row each, its check is the pick */
  ns_group_clear (b->dests);
  g_ptr_array_set_size (b->checks, 0);
  b->syncing = TRUE;
  for (guint i = 0; ds && i < json_array_get_length (ds); i++) {
    JsonObject *d = json_array_get_object_element (ds, i);
    const char *id = ns_json_str (d, "id"), *label = ns_json_str (d, "label");
    gboolean avail = json_object_get_boolean_member_with_default (d, "available", FALSE);
    const char *name = *label ? label : id;
    g_autoptr (GString) s = g_string_new (ns_json_str (d, "account"));
    JsonObject *r = last_of (b, id);
    g_autofree char *line = NULL;
    if (!avail)
      line = fill (b, text (b, "unavailable"), 0, 0, reason (b, ns_json_str (d, "why_id"), ns_json_str (d, "why")));
    else if (r && json_object_get_boolean_member_with_default (r, "ok", FALSE))
      line = fill (b, text (b, "last_ok"), json_object_get_int_member_with_default (r, "at", 0),
                   json_object_get_int_member_with_default (r, "repo_mb", 0), NULL);
    else if (r && *ns_json_str (r, "error"))
      line = fill (b, text (b, "last_failed"), json_object_get_int_member_with_default (r, "at", 0), 0,
                   ns_json_str (r, "error"));
    if (line)
      g_string_append_printf (s, "%s%s", s->len ? " • " : "", line);
    GtkWidget *row = ns_row (b->dests, "folder-remote-symbolic", "blue", name, s->str);
    GtkWidget *c = gtk_check_button_new ();
    gtk_check_button_set_active (GTK_CHECK_BUTTON (c), json_object_get_boolean_member_with_default (d, "picked", FALSE));
    gtk_widget_set_valign (c, GTK_ALIGN_CENTER);
    g_object_set_data_full (G_OBJECT (c), "ns-dest", g_strdup (id), g_free);
    g_signal_connect (c, "toggled", G_CALLBACK (toggled), b);
    ns_row_suffix (row, c);
    gtk_widget_set_sensitive (row, avail || gtk_check_button_get_active (GTK_CHECK_BUTTON (c)));
    g_ptr_array_add (b->checks, c);
  }
  b->syncing = FALSE;
  if (!ds || json_array_get_length (ds) == 0)   /* nothing to pick yet: where the destinations come from */
    ns_row (b->dests, NULL, NULL, text (b, "empty"), NULL);
}

static void open_accounts (GtkWidget *row, gpointer data) { ns_page_accounts (row, NULL); }

static void
now_started (JsonObject *root, const char *err, gpointer data)
{
  Bk *b = data;
  gtk_label_set_text (GTK_LABEL (b->msg), err ? err : text (b, "started"));
  gtk_widget_set_visible (b->msg, TRUE);
  if (err && b->poll_id) {   /* it did not start: nothing to wait for */
    g_source_remove (b->poll_id);
    b->poll_id = 0;
  }
}

static gboolean
poll_tick (gpointer data)
{
  bk_refresh (data);
  return G_SOURCE_CONTINUE;
}

static void
run_now (GtkWidget *row, gpointer data)
{
  Bk *b = data;
  static const char *const args[] = { "backup", "now", "--json", NULL };
  JsonObject *last = ns_json_obj (b->st, "last");
  b->started_at = last ? json_object_get_int_member_with_default (last, "at", 0) : 0;
  ns_mc_run (G_OBJECT (b->page), args, NULL, now_started, b);
  if (!b->poll_id)
    b->poll_id = g_timeout_add ((guint) (json_object_get_double_member_with_default (b->st, "refresh_s", 1) * 1000),
                                poll_tick, b);
}

static void
build (Bk *b)
{
  GtkWidget *g = ns_group (b->content, NULL);
  b->state = ns_row_info (g, "", "");
  b->dests = ns_group (b->content, text (b, "group"));
  b->note = ns_note (b->content, text (b, "note"));
  b->actions = ns_group (b->content, NULL);
  ns_row_button (b->actions, text (b, "link_account"), FALSE, open_accounts, b);
  ns_row_button (b->actions, text (b, "run_now"), FALSE, run_now, b);
  b->msg = ns_note (b->content, "");
  gtk_widget_set_visible (b->msg, FALSE);
  b->built = TRUE;
}

static void
got (JsonObject *root, const char *err, gpointer data)
{
  Bk *b = data;
  if (!root) {
    if (!b->built)
      ns_note (b->content, err ? err : "");
    return;
  }
  g_clear_pointer (&b->st, json_object_unref);
  b->st = json_object_ref (root);
  if (!b->built)
    build (b);
  if (!json_object_get_boolean_member_with_default (root, "ok", FALSE)) {
    gtk_label_set_text (GTK_LABEL (b->msg), ns_json_str (root, "error"));
    gtk_widget_set_visible (b->msg, TRUE);
    return;
  }
  show (b);
  /* a run started here: polled until the status has a newer run */
  JsonObject *last = ns_json_obj (root, "last");
  if (b->poll_id && last && json_object_get_int_member_with_default (last, "at", 0) > b->started_at) {
    g_source_remove (b->poll_id);
    b->poll_id = 0;
    gtk_widget_set_visible (b->msg, FALSE);
  }
}

static void
bk_refresh (Bk *b)
{
  static const char *const args[] = { "backup", "status", "--json", NULL };
  ns_mc_run (G_OBJECT (b->page), args, NULL, got, b);
}

void
ns_page_backup (GtkWidget *row, gpointer data)
{
  Bk *b = g_new0 (Bk, 1);
  b->checks = g_ptr_array_new ();
  b->page = ns_page_new ("Backup", &b->content);
  g_object_set_data_full (G_OBJECT (b->page), "ns-backup", b, (GDestroyNotify) bk_free);
  bk_refresh (b);
  ns_push (b->page);
}
