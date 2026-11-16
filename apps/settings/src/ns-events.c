/* Minerva > Eventi: the apps that may send events, the level of each of their channels, and the wake-ups Minerva
 * scheduled. Every text comes from minerva_ui.json (through `minerva-config events ui`); the data and every change
 * go through `minerva-config events … --json`, which talks to the minerva-events daemon. Nothing is guessed here:
 * a page with no data shows its message and nothing else. */
#include "ns-pages.h"
#include "ns-mc.h"
#include "ns-ui.h"

#include <json-glib/json-glib.h>

#define str_of ns_json_str
#define obj_of ns_json_obj
#define arr_of ns_json_arr

typedef struct {
  char *id, *label;
} Level;

typedef struct {
  char *id, *label, *level;
} Channel;

typedef struct {
  char *id, *name, *icon, *description, *status, *identity;
  GPtrArray *channels;   /* Channel* */
} App;

typedef struct Ev Ev;

/* one level picker of one channel: lives as long as the app page that shows it */
typedef struct {
  Ev    *ev;
  char  *app_id, *channel_id;
  GtkWidget *msg;
} Pick;

/* the consent of a pending app: Consenti / Rifiuta; the answer comes back from the daemon */
typedef struct {
  Ev    *ev;
  char  *app_id;
  gboolean allow;
  GtkWidget *msg;
} Decide;

/* one app of the list: the row's own data */
typedef struct {
  Ev   *ev;
  App  *app;
} AppRow;

struct Ev {
  GObject    *owner;
  AdwNavigationPage *page;
  GtkWidget  *content, *msg, *apps_g, *sched_g;
  GPtrArray  *levels;    /* Level* */
  GPtrArray  *apps;      /* App* */
  GPtrArray  *rows;      /* AppRow* */
  char       *allowed, *pending, *denied, *allow, *deny, *channels_label, *channel_one, *channel_many;
  char       *identity_label, *no_apps, *no_schedules, *notebook_label, *none_label;
  char       *cancel_label, *cancel_heading, *cancel_body, *cancelled, *saved, *error_label, *note_apps, *note_sched;
};

static void apps_load (Ev *ev);
static void schedules_load (Ev *ev);
static void decide_now (GtkWidget *row, gpointer data);
static void decide_free (Decide *d);

static void
level_free (Level *l)
{
  g_free (l->id);
  g_free (l->label);
  g_free (l);
}

static void
channel_free (Channel *c)
{
  g_free (c->id);
  g_free (c->label);
  g_free (c->level);
  g_free (c);
}

static void
app_free (App *a)
{
  g_free (a->id);
  g_free (a->name);
  g_free (a->icon);
  g_free (a->description);
  g_free (a->status);
  g_free (a->identity);
  g_ptr_array_free (a->channels, TRUE);
  g_free (a);
}

static void
pick_free (Pick *p)
{
  g_free (p->app_id);
  g_free (p->channel_id);
  g_free (p);
}

static void
app_row_free (AppRow *r)
{
  app_free (r->app);
  g_free (r);
}

static void
ev_free (Ev *ev)
{
  g_ptr_array_free (ev->levels, TRUE);
  g_ptr_array_free (ev->apps, TRUE);
  g_ptr_array_free (ev->rows, TRUE);
  g_free (ev->allowed);
  g_free (ev->pending);
  g_free (ev->denied);
  g_free (ev->allow);
  g_free (ev->deny);
  g_free (ev->channels_label);
  g_free (ev->channel_one);
  g_free (ev->channel_many);
  g_free (ev->identity_label);
  g_free (ev->no_apps);
  g_free (ev->no_schedules);
  g_free (ev->notebook_label);
  g_free (ev->none_label);
  g_free (ev->cancel_label);
  g_free (ev->cancel_heading);
  g_free (ev->cancel_body);
  g_free (ev->cancelled);
  g_free (ev->saved);
  g_free (ev->error_label);
  g_free (ev->note_apps);
  g_free (ev->note_sched);
  g_free (ev);
}

static gboolean
ok_of (JsonObject *root)
{
  return root && json_object_get_boolean_member_with_default (root, "ok", FALSE);
}

static void
say (GtkWidget *label, const char *text, gboolean error)
{
  gtk_label_set_text (GTK_LABEL (label), text ? text : "");
  gtk_widget_set_visible (label, text && *text);
  if (error)
    gtk_widget_add_css_class (label, "ns-danger");
  else
    gtk_widget_remove_css_class (label, "ns-danger");
}

static const char *
problem (JsonObject *root, const char *err, Ev *ev)
{
  if (err)
    return err;
  if (!root || !ok_of (root))
    return root ? str_of (root, "error") : ev->error_label;
  return NULL;
}

static const char *
status_label (Ev *ev, const char *status)
{
  if (!g_strcmp0 (status, "allowed"))
    return ev->allowed;
  if (!g_strcmp0 (status, "denied"))
    return ev->denied;
  return ev->pending;
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

/* ---- one app: its status, its channels and their levels ---- */

static void
level_saved (JsonObject *root, const char *err, gpointer data)
{
  Pick *p = data;
  const char *bad = problem (root, err, p->ev);
  say (p->msg, bad ? bad : p->ev->saved, bad != NULL);
}

static void
level_picked (GtkWidget *row, guint index, gpointer data)
{
  Pick *p = data;
  if (index >= p->ev->levels->len)
    return;
  Level *level = g_ptr_array_index (p->ev->levels, index);
  const char *args[] = { "events", "level", p->app_id, p->channel_id, level->id, "--json", NULL };
  ns_mc_run (p->ev->owner, args, NULL, level_saved, p);
}

static void
pick_page (AppRow *ar, App *app)
{
  Ev *ev = ar->ev;
  GtkWidget *c;
  AdwNavigationPage *page = ns_page_new (app->name, &c);
  GtkWidget *msg = ns_note (c, "");
  gtk_widget_set_visible (msg, FALSE);
  ns_row_info (ns_group (c, NULL), ev->identity_label, app->identity);
  if (*app->description)
    ns_note (c, app->description);
  if (g_strcmp0 (app->status, "allowed")) {
    GtkWidget *dg = ns_group (c, NULL);
    Decide *yes = g_new0 (Decide, 1);
    yes->ev = ev;
    yes->app_id = g_strdup (app->id);
    yes->allow = TRUE;
    yes->msg = msg;
    GtkWidget *rw = ns_row_button (dg, ev->allow, FALSE, decide_now, yes);
    g_object_set_data_full (G_OBJECT (rw), "ev-decide", yes, (GDestroyNotify) decide_free);
    Decide *no = g_new0 (Decide, 1);
    no->ev = ev;
    no->app_id = g_strdup (app->id);
    no->allow = FALSE;
    no->msg = msg;
    rw = ns_row_button (dg, ev->deny, TRUE, decide_now, no);
    g_object_set_data_full (G_OBJECT (rw), "ev-decide", no, (GDestroyNotify) decide_free);
  }

  GtkWidget *g = ns_group (c, ev->channels_label);
  for (guint i = 0; i < app->channels->len; i++) {
    Channel *ch = g_ptr_array_index (app->channels, i);
    g_autoptr (GPtrArray) labels = g_ptr_array_new ();
    guint selected = 0;
    for (guint k = 0; k < ev->levels->len; k++) {
      Level *level = g_ptr_array_index (ev->levels, k);
      g_ptr_array_add (labels, level->label);
      if (!g_strcmp0 (level->id, ch->level))
        selected = k;
    }
    g_ptr_array_add (labels, NULL);
    Pick *p = g_new0 (Pick, 1);
    p->ev = ev;
    p->app_id = g_strdup (app->id);
    p->channel_id = g_strdup (ch->id);
    p->msg = msg;
    GtkWidget *row = ns_row_pick (g, ch->label, (const char *const *) labels->pdata, selected, level_picked, p);
    g_object_set_data_full (G_OBJECT (row), "ev-pick", p, (GDestroyNotify) pick_free);
  }
  if (app->channels->len == 0)
    ns_note (c, ev->no_apps);
  ns_push (page);
}

static void
open_app (GtkWidget *row, gpointer data)
{
  AppRow *ar = data;
  pick_page (ar, ar->app);
}

static void
decided (JsonObject *root, const char *err, gpointer data)
{
  Decide *d = data;
  const char *bad = problem (root, err, d->ev);
  say (d->msg, bad ? bad : (d->allow ? d->ev->allowed : d->ev->denied), bad != NULL);
  if (!bad)
    apps_load (d->ev);
}

static void
decide_now (GtkWidget *row, gpointer data)
{
  Decide *d = data;
  const char *args[] = { "events", "decide", d->app_id, d->allow ? "allow" : "deny", "--json", NULL };
  ns_mc_run (d->ev->owner, args, NULL, decided, d);
}

static void
decide_free (Decide *d)
{
  g_free (d->app_id);
  g_free (d);
}

static void
app_shown (Ev *ev, App *app)
{
  AppRow *ar = g_new0 (AppRow, 1);
  ar->ev = ev;
  ar->app = app;
  g_ptr_array_add (ev->rows, ar);
  g_autofree char *status = g_strdup_printf ("%s · %u %s", status_label (ev, app->status),
                                            app->channels->len,
                                            app->channels->len == 1 ? ev->channel_one : ev->channel_many);
  GtkWidget *row = ns_row (ev->apps_g, "dialog-information-symbolic", "orange", app->name, status);
  ns_row_on_activate (row, open_app, ar);
}

static void
apps_loaded (JsonObject *root, const char *err, gpointer data)
{
  Ev *ev = data;
  const char *bad = problem (root, err, ev);
  if (bad) {
    say (ev->msg, bad, TRUE);
    return;
  }
  ns_group_clear (ev->apps_g);
  g_ptr_array_set_size (ev->rows, 0);
  g_ptr_array_set_size (ev->levels, 0);
  JsonArray *levels = arr_of (root, "levels");
  for (guint i = 0; levels && i < json_array_get_length (levels); i++) {
    JsonObject *o = json_array_get_object_element (levels, i);
    if (!o)
      continue;
    Level *level = g_new0 (Level, 1);
    level->id = g_strdup (str_of (o, "id"));
    level->label = g_strdup (str_of (o, "label"));
    g_ptr_array_add (ev->levels, level);
  }
  JsonArray *apps = arr_of (root, "apps");
  for (guint i = 0; apps && i < json_array_get_length (apps); i++) {
    JsonObject *o = json_array_get_object_element (apps, i);
    if (!o)
      continue;
    App *app = g_new0 (App, 1);
    app->id = g_strdup (str_of (o, "id"));
    app->name = g_strdup (str_of (o, "name"));
    app->icon = g_strdup (str_of (o, "icon"));
    app->description = g_strdup (str_of (o, "description"));
    app->status = g_strdup (str_of (o, "status"));
    app->identity = g_strdup (str_of (o, "identity"));
    app->channels = g_ptr_array_new_with_free_func ((GDestroyNotify) channel_free);
    JsonArray *channels = arr_of (o, "channels");
    for (guint k = 0; channels && k < json_array_get_length (channels); k++) {
      JsonObject *c = json_array_get_object_element (channels, k);
      if (!c)
        continue;
      Channel *ch = g_new0 (Channel, 1);
      ch->id = g_strdup (str_of (c, "id"));
      ch->label = g_strdup (str_of (c, "label"));
      ch->level = g_strdup (str_of (c, "level"));
      g_ptr_array_add (app->channels, ch);
    }
    g_ptr_array_add (ev->apps, app);
    app_shown (ev, app);
  }
  if (ev->apps->len == 0)
    ns_row (ev->apps_g, NULL, NULL, ev->no_apps, NULL);
}

static void
apps_load (Ev *ev)
{
  const char *args[] = { "events", "apps", "--json", NULL };
  ns_mc_run (ev->owner, args, NULL, apps_loaded, ev);
}

/* ---- the scheduled wake-ups ---- */

typedef struct {
  Ev   *ev;
  char *id;
  GtkWidget *msg;
} Cancel;

static void
cancel_free (Cancel *c)
{
  g_free (c->id);
  g_free (c);
}

static void
cancelled_done (JsonObject *root, const char *err, gpointer data)
{
  Cancel *c = data;
  const char *bad = problem (root, err, c->ev);
  if (bad)
    say (c->msg, bad, TRUE);
  else
    say (c->msg, c->ev->cancelled, FALSE);
  schedules_load (c->ev);
}

static void
cancel_answered (gboolean yes, gpointer data)
{
  Cancel *c = data;
  if (!yes)
    return;
  const char *args[] = { "events", "cancel", c->id, "--json", NULL };
  ns_mc_run (c->ev->owner, args, NULL, cancelled_done, c);
}

static void
cancel_now (GtkWidget *row, gpointer data)
{
  Cancel *c = data;
  const char *reason = g_object_get_data (G_OBJECT (row), "ev-reason");
  g_autofree char *body = fmt (c->ev->cancel_body, "reason", reason, NULL);
  ns_confirm (c->ev->content, c->ev->cancel_heading, body, c->ev->cancel_label, TRUE, cancel_answered, c);
}

static void
schedules_loaded (JsonObject *root, const char *err, gpointer data)
{
  Ev *ev = data;
  const char *bad = problem (root, err, ev);
  if (bad) {
    say (ev->msg, bad, TRUE);
    return;
  }
  ns_group_clear (ev->sched_g);
  JsonArray *entries = arr_of (root, "schedules");
  guint shown = 0;
  for (guint i = 0; entries && i < json_array_get_length (entries); i++) {
    JsonObject *o = json_array_get_object_element (entries, i);
    if (!o || g_strcmp0 (str_of (o, "status"), "pending"))
      continue;
    const char *reason = str_of (o, "reason");
    const char *notebook = str_of (o, "notebook");
    g_autofree char *subtitle = *notebook
        ? g_strdup_printf ("%s · %s: %s", reason, ev->notebook_label, notebook)
        : g_strdup (reason);
    GtkWidget *row = ns_row (ev->sched_g, "alarm-symbolic", "blue", str_of (o, "at_label"), subtitle);
    Cancel *c = g_new0 (Cancel, 1);
    c->ev = ev;
    c->id = g_strdup (str_of (o, "id"));
    c->msg = ev->msg;
    g_object_set_data_full (G_OBJECT (row), "ev-cancel", c, (GDestroyNotify) cancel_free);
    g_object_set_data_full (G_OBJECT (row), "ev-reason", g_strdup (reason), g_free);
    ns_row_on_activate (row, cancel_now, c);
    shown++;
  }
  if (shown == 0)
    ns_row (ev->sched_g, NULL, NULL, ev->no_schedules, NULL);
}

static void
schedules_load (Ev *ev)
{
  const char *args[] = { "events", "schedules", "--json", NULL };
  ns_mc_run (ev->owner, args, NULL, schedules_loaded, ev);
}

/* ---- the page ---- */

static void
ui_loaded (JsonObject *root, const char *err, gpointer data)
{
  Ev *ev = data;
  if (err || !ok_of (root)) {
    say (ev->msg, err ? err : ev->error_label, TRUE);
    return;
  }
  JsonObject *sections = obj_of (root, "sections");
  JsonObject *notes = obj_of (root, "notes");
  JsonObject *labels = obj_of (root, "labels");
  ev->allowed = g_strdup (str_of (labels, "allowed"));
  ev->pending = g_strdup (str_of (labels, "pending"));
  ev->denied = g_strdup (str_of (labels, "denied"));
  ev->allow = g_strdup (str_of (labels, "allow"));
  ev->deny = g_strdup (str_of (labels, "deny"));
  ev->channels_label = g_strdup (str_of (labels, "channels"));
  ev->channel_one = g_strdup (str_of (labels, "channel_one"));
  ev->channel_many = g_strdup (str_of (labels, "channel_many"));
  ev->identity_label = g_strdup (str_of (labels, "identity"));
  ev->no_apps = g_strdup (str_of (labels, "no_apps"));
  ev->no_schedules = g_strdup (str_of (labels, "no_schedules"));
  ev->notebook_label = g_strdup (str_of (labels, "notebook"));
  ev->none_label = g_strdup (str_of (labels, "none"));
  ev->cancel_label = g_strdup (str_of (labels, "cancel"));
  ev->cancel_heading = g_strdup (str_of (labels, "cancel_heading"));
  ev->cancel_body = g_strdup (str_of (labels, "cancel_body"));
  ev->cancelled = g_strdup (str_of (labels, "cancelled"));
  ev->saved = g_strdup (str_of (labels, "saved"));
  ev->error_label = g_strdup (str_of (labels, "error"));
  ev->note_apps = g_strdup (str_of (notes, "apps"));
  ev->note_sched = g_strdup (str_of (notes, "schedules"));
  if (*str_of (root, "title"))
    ns_page_set_title (ev->page, str_of (root, "title"));

  ev->apps_g = ns_group (ev->content, str_of (sections, "apps"));
  ns_note (ev->content, ev->note_apps);
  ev->sched_g = ns_group (ev->content, str_of (sections, "schedules"));
  ns_note (ev->content, ev->note_sched);
  apps_load (ev);
  schedules_load (ev);
}

void
ns_page_events (GtkWidget *row, gpointer data)
{
  GtkWidget *c;
  AdwNavigationPage *page = ns_page_new ("", &c);
  Ev *ev = g_new0 (Ev, 1);
  ev->page = page;
  ev->content = c;
  ev->owner = G_OBJECT (page);
  ev->levels = g_ptr_array_new_with_free_func ((GDestroyNotify) level_free);
  ev->apps = g_ptr_array_new_with_free_func ((GDestroyNotify) app_free);
  ev->rows = g_ptr_array_new_with_free_func ((GDestroyNotify) app_row_free);
  g_object_set_data_full (G_OBJECT (page), "ev", ev, (GDestroyNotify) ev_free);
  ev->msg = ns_note (c, "");
  gtk_widget_set_visible (ev->msg, FALSE);
  const char *args[] = { "events", "ui", "--json", NULL };
  ns_mc_run (ev->owner, args, NULL, ui_loaded, ev);
  ns_push (page);
}
